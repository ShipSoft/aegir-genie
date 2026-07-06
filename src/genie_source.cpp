// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: GPL-3.0-or-later

// genie_source.cpp — Phlex source plugin embedding the GENIE event generator
//
// Provides MCParticle vectors from GENIE neutrino interactions generated
// in situ: a genie::GMCJDriver convolves the SHiP neutrino flux (schema-v1
// ntuple, see ship_flux_driver.hpp) with cross-section splines and a TGeo
// geometry imported from the same GDML the Geant4 simulation uses, so
// interaction vertices are placed consistently with the tracked geometry
// (the library-embedding approach of nutools/GENIEHelper and gSeaGen).
//
// GENIE is not thread-safe (unsynchronized singletons, global gRandom /
// gGeoManager), so this source runs with phlex::concurrency::serial and one
// instance per process. Per-event reproducibility is preserved by reseeding
// genie::RandomGen from a Philox-derived per-event seed; RandomGen::SetSeed
// also reseeds ROOT's global gRandom, covering both streams GENIE draws
// from (this mirrors what nutools' GENIEHelper achieves by swapping
// gRandom).
//
// Initialization order matters (cf. nugen/GENIEHelper): the tune must be
// set and built before anything touches PDGLibrary; splines load after the
// tune; the geometry analyzer and flux driver attach to the GMCJDriver last.

#include <TGeoManager.h>

#include <SHiP/MCParticle.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Framework/Conventions/Units.h"
#include "Framework/EventGen/EventRecord.h"
#include "Framework/EventGen/GMCJDriver.h"
#include "Framework/GHEP/GHepParticle.h"
#include "Framework/GHEP/GHepStatus.h"
#include "Framework/Numerical/RandomGen.h"
#include "Framework/Utils/AppInit.h"
#include "Framework/Utils/RunOpt.h"
#include "Tools/Geometry/ROOTGeomAnalyzer.h"
#include "genie_config.hpp"
#include "mc_particle_source.hpp"
#include "philox_rng.hpp"
#include "ship_flux_driver.hpp"

namespace {

// Philox stream selector for the GENIE reseeding, distinct from the other
// aegir generators (see philox_rng.hpp).
constexpr std::uint32_t kGenieStream = 0x47454E49;  // "GENI"

class GenieSource : public phlex::source {
 public:
  explicit GenieSource(aegir::GenieSourceConfig cfg) : cfg_{std::move(cfg)} {
    cfg_.validate();

    // 1. Tune selection — before any access to PDGLibrary or AlgFactory.
    auto* run_opt = genie::RunOpt::Instance();
    run_opt->SetTuneName(cfg_.tune);
    if (!run_opt->Tune())
      throw std::runtime_error("genie_source: tune '" + cfg_.tune +
                               "' not recognised by GENIE");
    run_opt->BuildTune();

    // 2. Tame GENIE's logging (ships with the GENIE config files; resolved
    //    via $GENIE/config).
    genie::utils::app_init::MesgThresholds("Messenger_whisper.xml");

    // 3. Base RNG seed; generate() re-seeds per event.
    genie::utils::app_init::RandGen(cfg_.seed);

    // 4. Cross-section splines. XSecTable exit()s the process when the file
    //    is unusable, so cfg_.validate() checked its existence up front.
    genie::utils::app_init::XSecTable(cfg_.spline_file, true);

    // 5. Geometry: the same GDML the Geant4 side tracks through, imported
    //    into TGeo (lengths in cm, ROOT's native unit).
    auto* geo = TGeoManager::Import(cfg_.gdml_file.c_str());
    if (!geo)
      throw std::runtime_error("genie_source: TGeoManager::Import failed on '" +
                               cfg_.gdml_file + "'");
    geom_ = std::make_unique<genie::geometry::ROOTGeomAnalyzer>(geo);
    geom_->SetLengthUnits(genie::units::centimeter);
    geom_->SetDensityUnits(genie::units::gram_centimeter3);
    geom_->SetTopVolName(cfg_.top_volume);

    // 6. Flux: cycle so KeepOnThrowingFluxNeutrinos always finds a ray; the
    //    delivered POT is still accounted through GFluxExposureI.
    flux_ = std::make_unique<aegir::ShipFluxDriver>(cfg_.flux_file,
                                                    /*cycle=*/true);

    // 7. MC job driver.
    driver_ = std::make_unique<genie::GMCJDriver>();
    driver_->UseFluxDriver(flux_.get());
    driver_->UseGeomAnalyzer(geom_.get());
    driver_->UseSplines();
    driver_->ForceSingleProbScale();  // unweighted events
    driver_->KeepOnThrowingFluxNeutrinos(true);  // one interaction per event

    // The max-path-lengths geometry scan is expensive; cache it as XML when
    // the config names a file.
    bool const use_cached = !cfg_.max_path_lengths_file.empty() &&
                            std::filesystem::exists(cfg_.max_path_lengths_file);
    if (use_cached && !driver_->UseMaxPathLengths(cfg_.max_path_lengths_file))
      throw std::runtime_error(
          "genie_source: failed to load max path lengths from '" +
          cfg_.max_path_lengths_file + "' — delete the file to recompute it");

    driver_->Configure();

    if (!cfg_.max_path_lengths_file.empty() && !use_cached)
      geom_->GetMaxPathLengths().SaveAsXml(cfg_.max_path_lengths_file);
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    reseed(static_cast<std::uint32_t>(id.number()));

    std::unique_ptr<genie::EventRecord> event{driver_->GenerateEvent()};
    if (!event)
      throw std::runtime_error(
          "genie_source: GMCJDriver::GenerateEvent returned no event for "
          "event " +
          std::to_string(id.number()));

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

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial);
  }

  phlex::index_generator indices() override { co_return; }

 private:
  // Derive a per-event seed from (base seed, event number) with Philox so
  // re-running the same event yields the same GENIE draws regardless of what
  // was generated before. RandomGen::SetSeed also reseeds gRandom.
  void reseed(std::uint32_t event_number) {
    aegir::PhiloxRng rng{static_cast<std::uint32_t>(cfg_.seed) ^ event_number,
                         kGenieStream};
    // TRandom3 seeds are UInt_t; keep the full 32-bit range but avoid 0
    // (which TRandom3 interprets as "seed from clock").
    auto const seed = 1 + static_cast<long>(rng.uniform() * 4294967294.0);
    genie::RandomGen::Instance()->SetSeed(seed);
  }

  aegir::GenieSourceConfig cfg_;
  std::unique_ptr<genie::geometry::ROOTGeomAnalyzer> geom_;
  std::unique_ptr<aegir::ShipFluxDriver> flux_;
  std::unique_ptr<genie::GMCJDriver> driver_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  aegir::GenieSourceConfig cfg;
  cfg.tune = config.get<std::string>("tune", std::string{"G18_02a_00_000"});
  cfg.spline_file = config.get<std::string>("splines");
  cfg.flux_file = config.get<std::string>("flux_file");
  cfg.gdml_file = config.get<std::string>("gdml_file");
  cfg.top_volume =
      config.get<std::string>("top_volume", std::string{"World"});
  cfg.seed = config.get<long>("seed", 20260706L);
  cfg.max_path_lengths_file =
      config.get<std::string>("max_path_lengths_file", std::string{});

  s.add_source<GenieSource>("genie", std::move(cfg));
}
