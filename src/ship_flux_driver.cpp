// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ship_flux_driver.hpp"

#include <ROOT/RNTupleReader.hxx>
#include <ROOT/RNTupleView.hxx>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace aegir {

namespace {

constexpr std::int32_t kSupportedSchemaVersion = 1;

// mm -> m, ns -> s: GENIE flux positions are expected in SI units.
constexpr double kMm2M = 1e-3;
constexpr double kNs2S = 1e-9;

}  // namespace

// Bundles the RNTuple reader with the typed field views (views are not
// default-constructible, hence the factory-style construction below).
struct ShipFluxDriver::Reader {
  std::unique_ptr<ROOT::RNTupleReader> flux;
  ROOT::RNTupleView<std::int32_t> pdg;
  ROOT::RNTupleView<double> vx, vy, vz, t;
  ROOT::RNTupleView<double> px, py, pz;
  ROOT::RNTupleView<double> weight;
};

ShipFluxDriver::ShipFluxDriver(std::string const& path, bool cycle)
    : genie::flux::GFluxExposureI(genie::flux::kPOTs),
      path_{path},
      cycle_{cycle} {
  // --- metadata -------------------------------------------------------------
  std::unique_ptr<ROOT::RNTupleReader> meta;
  try {
    meta = ROOT::RNTupleReader::Open("flux_meta", path);
  } catch (std::exception const& e) {
    throw std::runtime_error(
        "ship_flux_driver: cannot open RNTuple 'flux_meta' in '" + path +
        "' — is this a SHiP neutrino flux file (docs/neutrino_flux.md)? (" +
        e.what() + ")");
  }
  if (meta->GetNEntries() != 1)
    throw std::runtime_error("ship_flux_driver: 'flux_meta' in '" + path +
                             "' holds " + std::to_string(meta->GetNEntries()) +
                             " entries, expected exactly 1");
  {
    auto version = meta->GetView<std::int32_t>("schema_version");
    auto pot = meta->GetView<double>("pot");
    auto max_energy = meta->GetView<double>("max_energy");
    if (version(0) != kSupportedSchemaVersion)
      throw std::runtime_error(
          "ship_flux_driver: '" + path + "' declares schema version " +
          std::to_string(version(0)) + "; this driver supports version " +
          std::to_string(kSupportedSchemaVersion));
    pot_ = pot(0);
    max_energy_ = max_energy(0);
  }
  if (max_energy_ <= 0.0)
    throw std::runtime_error(
        "ship_flux_driver: '" + path + "' declares max_energy = " +
        std::to_string(max_energy_) + " GeV (must be > 0)");

  // --- flux rays ------------------------------------------------------------
  std::unique_ptr<ROOT::RNTupleReader> flux;
  try {
    flux = ROOT::RNTupleReader::Open("nu_flux", path);
  } catch (std::exception const& e) {
    throw std::runtime_error(
        "ship_flux_driver: cannot open RNTuple 'nu_flux' in '" + path + "' (" +
        e.what() + ")");
  }
  n_entries_ = flux->GetNEntries();
  if (n_entries_ == 0)
    throw std::runtime_error("ship_flux_driver: '" + path +
                             "' contains no flux entries");

  try {
    auto& r = *flux;
    reader_ = std::make_unique<Reader>(Reader{
        .flux = nullptr,  // filled below, after all views are created
        .pdg = r.GetView<std::int32_t>("pdg"),
        .vx = r.GetView<double>("vx"),
        .vy = r.GetView<double>("vy"),
        .vz = r.GetView<double>("vz"),
        .t = r.GetView<double>("t"),
        .px = r.GetView<double>("px"),
        .py = r.GetView<double>("py"),
        .pz = r.GetView<double>("pz"),
        .weight = r.GetView<double>("weight"),
    });
    reader_->flux = std::move(flux);
  } catch (std::exception const& e) {
    throw std::runtime_error(
        "ship_flux_driver: field missing from 'nu_flux' in '" + path +
        "' — schema v1 requires pdg, vx, vy, vz, t, px, py, pz, weight (" +
        e.what() + ")");
  }

  // Total statistical weight, for the exposure accounting: a ray of weight w
  // stands for w rays' worth of flux, so delivered POT scales with the
  // weight consumed, not the entry count — the two differ mid-cycle for
  // files with non-uniform weights (merged samples). One startup scan over
  // the columnar weight field.
  for (std::uint64_t i = 0; i < n_entries_; ++i)
    total_weight_ += reader_->weight(i);
  if (!(total_weight_ > 0.0))
    throw std::runtime_error("ship_flux_driver: flux weights in '" + path +
                             "' sum to " + std::to_string(total_weight_) +
                             " (must be > 0)");
}

ShipFluxDriver::~ShipFluxDriver() = default;

genie::PDGCodeList const& ShipFluxDriver::FluxParticles() {
  if (!pdg_list_built_) {
    // One scan over the (columnar, compressed) pdg field; deduplicate first
    // so PDGCodeList's PDG-library validation runs once per flavour.
    std::set<std::int32_t> codes;
    for (std::uint64_t i = 0; i < n_entries_; ++i)
      codes.insert(reader_->pdg(i));
    for (auto code : codes) pdg_list_.push_back(code);
    // PDGCodeList::push_back silently skips codes PDGLibrary does not know
    // (a GENIE log line only); rays with such codes would still be served to
    // a GMCJDriver that configured no generator for them, so fail loudly.
    if (pdg_list_.size() != codes.size()) {
      std::string dropped;
      for (auto code : codes)
        if (!pdg_list_.ExistsInPDGCodeList(code))
          dropped += (dropped.empty() ? "" : ", ") + std::to_string(code);
      throw std::runtime_error(
          "ship_flux_driver: '" + path_ +
          "' contains PDG code(s) unknown to GENIE's PDGLibrary: " + dropped);
    }
    pdg_list_built_ = true;
  }
  return pdg_list_;
}

bool ShipFluxDriver::GenerateNext() {
  if (end_) return false;
  auto next = static_cast<std::uint64_t>(index_ + 1);
  if (next >= n_entries_) {
    if (!cycle_) {
      end_ = true;
      return false;
    }
    next = 0;
  }

  auto& r = *reader_;  // RNTupleView::operator() is non-const
  pdg_ = r.pdg(next);
  weight_ = r.weight(next);
  double const px = r.px(next);
  double const py = r.py(next);
  double const pz = r.pz(next);
  // Massless neutrinos: E = |p|.
  p4_.SetPxPyPzE(px, py, pz, std::sqrt(px * px + py * py + pz * pz));
  x4_.SetXYZT(r.vx(next) * kMm2M, r.vy(next) * kMm2M, r.vz(next) * kMm2M,
              r.t(next) * kNs2S);

  index_ = static_cast<long int>(next);
  ++n_used_;
  used_weight_ += weight_;
  return true;
}

ShipFluxDriver::Ray ShipFluxDriver::peek_first_ray() {
  auto& r = *reader_;  // RNTupleView::operator() is non-const
  Ray ray;
  ray.pdg = r.pdg(0);
  double const px = r.px(0);
  double const py = r.py(0);
  double const pz = r.pz(0);
  ray.momentum.SetPxPyPzE(px, py, pz, std::sqrt(px * px + py * py + pz * pz));
  ray.position.SetXYZT(r.vx(0) * kMm2M, r.vy(0) * kMm2M, r.vz(0) * kMm2M,
                       r.t(0) * kNs2S);
  return ray;
}

void ShipFluxDriver::Clear(Option_t* /*opt*/) {
  // Reset the driver state: rewind to the start of the file and drop the
  // accumulated exposure. GMCJDriver / ROOTGeomAnalyzer call this with
  // option "CycleHistory" after the max-path-length flux scan, precisely so
  // that scan rays do not count towards the delivered POT.
  index_ = -1;
  end_ = false;
  n_used_ = 0;
  used_weight_ = 0.0;
}

void ShipFluxDriver::GenerateWeighted(bool gen_weighted) {
  // The rays in the file are inherently weighted (merged samples with
  // different POT equivalents). Unweighted generation would need an
  // accept–reject on the weight; until that is implemented, Weight() always
  // reports the file weight and downstream consumers must account for it
  // (or use flux files with unit weights).
  gen_weighted_ = gen_weighted;
}

double ShipFluxDriver::GetTotalExposure() const {
  // The whole file corresponds to pot_ protons on target; scale by the
  // fraction of the total statistical weight consumed (for uniform weights
  // this reduces to the entry-count fraction). Cycling accumulates
  // full-file multiples. total_weight_ > 0 is enforced at construction.
  return pot_ * used_weight_ / total_weight_;
}

}  // namespace aegir
