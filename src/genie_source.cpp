// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// genie_source.cpp — Phlex source plugin embedding the GENIE event generator
//
// Provides MCParticle vectors from GENIE neutrino interactions generated
// in situ: a genie::GMCJDriver convolves the SHiP neutrino flux (schema-v1
// ntuple via ShipFluxDriver, or a GENIE GSimple flux file — see the
// flux_format config key) with cross-section splines and the GeoModel
// geometry the Geant4 simulation tracks through (via ShipGeomAnalyzer), so
// interaction vertices are placed consistently with the tracked geometry
// (the library-embedding approach of nutools/GENIEHelper and gSeaGen).
//
// The driver assembly is shared with the standalone gevgen_ship app (see
// genie_driver_setup.hpp), which writes native GHEP output for validating
// this source through the gntpc/genie_reader_source path.
//
// GENIE is not thread-safe (unsynchronized singletons, global gRandom /
// gGeoManager), so this source runs with phlex::concurrency::serial and one
// instance per process. Per-event reproducibility is preserved by reseeding
// genie::RandomGen from a Philox-derived per-event seed (see
// aegir::reseed_event).

#include <SHiP/MCParticle.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Framework/EventGen/EventRecord.h"
#include "Framework/EventGen/GMCJDriver.h"
#include "Framework/GHEP/GHepParticle.h"
#include "Framework/GHEP/GHepStatus.h"
#include "genie_config.hpp"
#include "genie_driver_setup.hpp"
#include "mc_particle_source.hpp"

namespace {

class GenieSource : public phlex::source {
 public:
  explicit GenieSource(aegir::GenieSourceConfig cfg)
      : cfg_{std::move(cfg)},
        bundle_{aegir::make_genie_driver(cfg_, "genie_source")} {}

  // Phlex may dispatch generate() calls out of event-number order (serial
  // concurrency guarantees mutual exclusion, not FIFO — observed in practice
  // as occasional swaps under TBB scheduling). The flux driver is shared
  // sequential state, so generation must happen in event-number order for
  // determinism and for identity with the strictly-ordered gevgen_ship app:
  // generate ahead in order when a later event is requested first, cache the
  // results, and serve earlier requests from the cache. The cache stays as
  // small as the scheduler's lookahead. Assumes 0-based contiguous event
  // numbering (generate_layers' default starting_number).
  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto const requested = id.number();
    if (requested >= next_in_order_) {
      if (requested - next_in_order_ > kMaxLookahead)
        throw std::runtime_error(
            "genie_source: event " + std::to_string(requested) +
            " requested while event " + std::to_string(next_in_order_) +
            " is next in order — event numbering does not look 0-based and "
            "contiguous (generate_layers starting_number 0)");
      while (next_in_order_ <= requested) {
        ready_.emplace(next_in_order_, generate_in_order(next_in_order_));
        ++next_in_order_;
      }
    }
    auto const it = ready_.find(requested);
    if (it == ready_.end())
      throw std::runtime_error("genie_source: event " +
                               std::to_string(requested) +
                               " requested twice — cannot regenerate without "
                               "disturbing the flux sequence");
    auto out = std::move(it->second);
    ready_.erase(it);
    return out;
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial);
  }

  phlex::index_generator indices() override { co_return; }

 private:
  std::vector<SHiP::MCParticle> generate_in_order(std::size_t event_number) {
    aegir::reseed_event(cfg_.seed, static_cast<std::uint32_t>(event_number));

    std::unique_ptr<genie::EventRecord> event{bundle_.driver->GenerateEvent()};
    if (!event)
      throw std::runtime_error(
          "genie_source: GMCJDriver::GenerateEvent returned no event for "
          "event " +
          std::to_string(event_number));
    aegir::trace_event(static_cast<std::uint32_t>(event_number), *event,
                       *bundle_.flux);

    // Interaction vertex in detector coordinates, SI units (m, s); the
    // per-particle X4() positions are nuclear-scale offsets relative to the
    // hit nucleus and irrelevant for tracking.
    auto const* vtx = event->Vertex();
    std::array<double, 3> const vertex{vtx->X() * 1e3, vtx->Y() * 1e3,
                                       vtx->Z() * 1e3};  // m -> mm
    double const time = vtx->T() * 1e9;                  // s -> ns

    auto const n = event->GetEntries();
    std::vector<SHiP::MCParticle> particles;
    particles.reserve(static_cast<std::size_t>(n));
    // GHEP-record index -> output index for written (final-state) particles.
    std::vector<int> out_index(static_cast<std::size_t>(n), -1);

    for (int i = 0; i < n; ++i) {
      auto const* p = event->Particle(i);
      if (!p || p->Status() != genie::kIStStableFinalState) continue;

      out_index[static_cast<std::size_t>(i)] =
          static_cast<int>(particles.size());

      SHiP::MCParticle mc;
      mc.pdgCode = p->Pdg();
      mc.vertex = vertex;
      mc.momentum = {p->Px(), p->Py(), p->Pz()};  // GeV
      mc.energy = p->E();                         // GeV
      mc.time = time;
      mc.motherId = p->FirstMother();  // remapped below
      mc.status = 1;
      particles.push_back(mc);
    }

    // Remap motherId from the full GHEP record to the emitted collection, or
    // -1 when the mother was not itself written out — the common case, since
    // mothers of final-state particles (the probe, the struck nucleus,
    // intermediate states) are generally not final state.
    for (auto& mc : particles) {
      int const m = mc.motherId;
      mc.motherId =
          (m >= 0 && m < n) ? out_index[static_cast<std::size_t>(m)] : -1;
    }
    return particles;
  }

  aegir::GenieSourceConfig cfg_;
  aegir::GenieDriverBundle bundle_;  // initialized after cfg_ (declared last)

  // In-order generation bookkeeping (see generate()); only touched from the
  // serialized provider function, so no synchronisation is needed.
  static constexpr std::size_t kMaxLookahead = 10000;
  std::size_t next_in_order_ = 0;
  std::map<std::size_t, std::vector<SHiP::MCParticle>> ready_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  aegir::GenieSourceConfig cfg;
  cfg.tune = config.get<std::string>("tune", std::string{"G18_02a_00_000"});
  cfg.spline_file = config.get<std::string>("splines");
  cfg.flux_file = config.get<std::string>("flux_file");
  cfg.flux_format = config.get<std::string>("flux_format", std::string{"ship"});
  cfg.geometry_file = config.get<std::string>("geometry_file");
  cfg.top_volume = config.get<std::string>("top_volume", std::string{});
  cfg.seed = config.get<long>("seed", 20260706L);
  cfg.max_path_lengths_file =
      config.get<std::string>("max_path_lengths_file", std::string{});

  s.add_source<GenieSource>("genie", std::move(cfg));
}
