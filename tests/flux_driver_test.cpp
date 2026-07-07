// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// flux_driver_test.cpp — standalone check of the flux drivers and the
// genie_source config validation
//
// Writes synthetic flux files with known values — a schema-v1 SHiP ntuple
// and a GENIE GSimple file — then drives ShipFluxDriver / GSimpleNtpFlux
// through them and checks unit conversions, exhaustion/cycling behaviour
// and exposure accounting. Deliberately does not involve GMCJDriver, so it
// runs without cross-section splines.

#include <TFile.h>
#include <TSystem.h>
#include <TTree.h>

#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleWriter.hxx>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Tools/Flux/GSimpleNtpFlux.h"
#include "genie_config.hpp"
#include "ship_flux_driver.hpp"

namespace {

int failures = 0;

void check(bool ok, std::string const& what) {
  if (ok) {
    std::cout << "  PASS: " << what << '\n';
  } else {
    std::cout << "  FAIL: " << what << '\n';
    ++failures;
  }
}

void check_close(double a, double b, std::string const& what,
                 double tol = 1e-12) {
  check(std::abs(a - b) <= tol * std::max({1.0, std::abs(a), std::abs(b)}),
        what + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")");
}

struct Ray {
  std::int32_t pdg;
  double vx, vy, vz, t;  // mm, ns
  double px, py, pz;     // GeV
  double weight;
};

// Three rays with easily recognisable values.
std::vector<Ray> const kRays{
    {14, 1000.0, -2000.0, -45000.0, 5.0, 0.1, -0.2, 20.0, 1.0},
    {-14, 0.0, 0.0, -42000.0, 0.0, 0.0, 0.0, 35.0, 0.424},
    {12, 500.0, 250.0, -48000.0, 2.5, -0.05, 0.05, 8.0, 1.0},
};
constexpr double kPot = 6.5041e10;
constexpr double kMaxEnergy = 35.001;
double const kTotalWeight = [] {
  double sum = 0.0;
  for (auto const& ray : kRays) sum += ray.weight;
  return sum;
}();

void write_flux_file(std::string const& path) {
  {
    auto model = ROOT::RNTupleModel::Create();
    auto pdg = model->MakeField<std::int32_t>("pdg");
    auto vx = model->MakeField<double>("vx");
    auto vy = model->MakeField<double>("vy");
    auto vz = model->MakeField<double>("vz");
    auto t = model->MakeField<double>("t");
    auto px = model->MakeField<double>("px");
    auto py = model->MakeField<double>("py");
    auto pz = model->MakeField<double>("pz");
    auto weight = model->MakeField<double>("weight");
    auto parent_pdg = model->MakeField<std::int32_t>("parent_pdg");
    auto parent_px = model->MakeField<double>("parent_px");
    auto parent_py = model->MakeField<double>("parent_py");
    auto parent_pz = model->MakeField<double>("parent_pz");
    auto process_id = model->MakeField<std::int32_t>("process_id");
    auto origin_run = model->MakeField<std::int64_t>("origin_run");
    auto origin_event = model->MakeField<std::int64_t>("origin_event");

    auto writer =
        ROOT::RNTupleWriter::Recreate(std::move(model), "nu_flux", path);
    std::int64_t i = 0;
    for (auto const& ray : kRays) {
      *pdg = ray.pdg;
      *vx = ray.vx;
      *vy = ray.vy;
      *vz = ray.vz;
      *t = ray.t;
      *px = ray.px;
      *py = ray.py;
      *pz = ray.pz;
      *weight = ray.weight;
      *parent_pdg = 211;
      *parent_px = 0.0;
      *parent_py = 0.0;
      *parent_pz = 2.0 * ray.pz;
      *process_id = 0;
      *origin_run = 1000;
      *origin_event = i++;
      writer->Fill();
    }
  }
  {
    auto model = ROOT::RNTupleModel::Create();
    auto version = model->MakeField<std::int32_t>("schema_version");
    auto pot = model->MakeField<double>("pot");
    auto max_energy = model->MakeField<double>("max_energy");
    auto description = model->MakeField<std::string>("description");
    auto software = model->MakeField<std::string>("software");

    std::unique_ptr<TFile> file{TFile::Open(path.c_str(), "UPDATE")};
    auto writer =
        ROOT::RNTupleWriter::Append(std::move(model), "flux_meta", *file);
    *version = 1;
    *pot = kPot;
    *max_energy = kMaxEnergy;
    *description = "synthetic flux for flux_driver_test";
    *software = "aegir-genie flux_driver_test";
    writer->Fill();
  }
}

void test_metadata_and_conversions(std::string const& path) {
  std::cout << "metadata and unit conversions:\n";
  aegir::ShipFluxDriver driver{path};

  check_close(driver.MaxEnergy(), kMaxEnergy, "MaxEnergy from flux_meta");
  check_close(driver.pot(), kPot, "POT from flux_meta");
  check(driver.entries() == kRays.size(), "entry count");
  check(!driver.End(), "End() false before first ray");
  check(driver.NFluxNeutrinos() == 0, "no rays used yet");
  check_close(driver.GetTotalExposure(), 0.0, "no exposure yet");

  check(driver.GenerateNext(), "GenerateNext() first ray");
  auto const& ray = kRays.front();
  check(driver.PdgCode() == ray.pdg, "pdg code");
  check_close(driver.Weight(), ray.weight, "weight");
  check(driver.Index() == 0, "index of first ray");
  check_close(driver.GetTotalExposure(), kPot * ray.weight / kTotalWeight,
              "exposure scales with the weight consumed, not the ray count");
  auto const& x4 = driver.Position();
  check_close(x4.X(), ray.vx * 1e-3, "vx mm -> m");
  check_close(x4.Y(), ray.vy * 1e-3, "vy mm -> m");
  check_close(x4.Z(), ray.vz * 1e-3, "vz mm -> m");
  check_close(x4.T(), ray.t * 1e-9, "t ns -> s");
  auto const& p4 = driver.Momentum();
  check_close(p4.Px(), ray.px, "px passthrough (GeV)");
  check_close(p4.Py(), ray.py, "py passthrough (GeV)");
  check_close(p4.Pz(), ray.pz, "pz passthrough (GeV)");
  check_close(p4.E(),
              std::sqrt(ray.px * ray.px + ray.py * ray.py + ray.pz * ray.pz),
              "E = |p| (massless)");
}

void test_exhaustion_and_clear(std::string const& path) {
  std::cout << "exhaustion and Clear():\n";
  aegir::ShipFluxDriver driver{path};

  int generated = 0;
  while (driver.GenerateNext()) ++generated;
  check(generated == static_cast<int>(kRays.size()),
        "one ray per entry without cycling");
  check(driver.End(), "End() true after exhaustion");
  check(!driver.GenerateNext(), "GenerateNext() keeps returning false");
  check(driver.NFluxNeutrinos() == static_cast<long>(kRays.size()),
        "ray count");
  check_close(driver.GetTotalExposure(), kPot, "full-file exposure = pot");

  driver.Clear("CycleHistory");
  check(!driver.End(), "Clear() rewinds");
  check(driver.NFluxNeutrinos() == 0,
        "Clear() drops the exposure history (max-path-length scan rays must "
        "not count towards POT)");
  check(driver.GenerateNext(), "GenerateNext() works again after Clear()");
  check(driver.Index() == 0, "rewound to first entry");
  check(driver.NFluxNeutrinos() == 1, "exposure accumulates afresh");
  check_close(driver.GetTotalExposure(),
              kPot * kRays.front().weight / kTotalWeight,
              "weight accounting restarts after Clear()");
}

void test_cycling(std::string const& path) {
  std::cout << "cycling:\n";
  aegir::ShipFluxDriver driver{path, /*cycle=*/true};

  auto const twice = 2 * kRays.size();
  for (std::size_t i = 0; i < twice; ++i) {
    if (!driver.GenerateNext()) {
      check(false, "GenerateNext() with cycling never ends");
      return;
    }
  }
  check(driver.Index() == static_cast<long>(kRays.size() - 1),
        "wrapped to the correct entry");
  check(!driver.End(), "End() stays false when cycling");
  check_close(driver.GetTotalExposure(), 2.0 * kPot,
              "two passes = twice the POT");
}

void test_flux_particles(std::string const& path) {
  std::cout << "FluxParticles():\n";
  // PDGCodeList validates codes against GENIE's PDGLibrary, which needs the
  // $GENIE data directory (set by the genie conda package's activation).
  if (!gSystem->Getenv("GENIE")) {
    std::cout << "  SKIP: $GENIE not set — run under `pixi run test`\n";
    return;
  }
  aegir::ShipFluxDriver driver{path};
  auto const& list = driver.FluxParticles();
  check(list.size() == 3, "three distinct flavours (12, 14, -14)");
  check(list.ExistsInPDGCodeList(14) && list.ExistsInPDGCodeList(-14) &&
            list.ExistsInPDGCodeList(12),
        "expected flavours present");
}

void test_bad_files() {
  std::cout << "error handling:\n";
  try {
    aegir::ShipFluxDriver driver{"/nonexistent/flux.root"};
    check(false, "missing file throws");
  } catch (std::runtime_error const& e) {
    check(std::string{e.what()}.find("flux_meta") != std::string::npos,
          "missing file throws with a helpful message");
  }
}

// --- GSimple flux (GENIE's own driver; used for flux_format 'gsimple') ----

// GSimple entries are lab-frame meters/seconds (already SI) with explicit E;
// mirror kRays so both drivers can be checked against the same expectations.
void write_gsimple_file(std::string const& path) {
  std::unique_ptr<TFile> file{TFile::Open(path.c_str(), "RECREATE")};
  auto* fluxtree = new TTree("flux", "GSimple flux");
  auto* entry = new genie::flux::GSimpleNtpEntry;
  fluxtree->Branch("entry", &entry);
  for (auto const& ray : kRays) {
    entry->Reset();
    entry->pdg = ray.pdg;
    entry->wgt = 1.0;  // min=max=1 -> driver takes the already-unweighted path
    entry->vtxx = ray.vx * 1e-3;  // store as meters
    entry->vtxy = ray.vy * 1e-3;
    entry->vtxz = ray.vz * 1e-3;
    entry->vtxt = ray.t * 1e-9;  // seconds
    entry->px = ray.px;
    entry->py = ray.py;
    entry->pz = ray.pz;
    entry->E = std::sqrt(ray.px * ray.px + ray.py * ray.py + ray.pz * ray.pz);
    entry->metakey = 42;
    fluxtree->Fill();
  }
  auto* metatree = new TTree("meta", "GSimple flux meta");
  auto* meta = new genie::flux::GSimpleNtpMeta;
  metatree->Branch("meta", &meta);
  meta->Reset();
  meta->maxEnergy = kMaxEnergy;
  meta->minWgt = 1.0;
  meta->maxWgt = 1.0;
  meta->protons = kPot;
  meta->pdglist = {12, 14, -14};
  meta->metakey = 42;
  metatree->Fill();
  file->Write();
  file->Close();
}

void test_gsimple_driver(std::string const& path) {
  std::cout << "GSimpleNtpFlux (flux_format 'gsimple'):\n";
  // The driver registers flavours in a PDGCodeList, which validates against
  // PDGLibrary and therefore needs the $GENIE data directory.
  if (!gSystem->Getenv("GENIE")) {
    std::cout << "  SKIP: $GENIE not set — run under `pixi run test`\n";
    return;
  }
  write_gsimple_file(path);

  genie::flux::GSimpleNtpFlux driver;
  // "no-offset-index": start at entry 0 instead of a random offset — the
  // same configuration genie_driver_setup uses.
  driver.LoadBeamSimData(path, "no-offset-index");
  // Quirk: GSimpleNtpFlux's cycle counter starts at 0 and wraps while
  // fICycle < fNCycles, so SetNumOfCycles(N) delivers N+1 passes; N=1 means
  // two passes before End(). Only 0 (= cycle forever, what
  // genie_driver_setup uses) has unsurprising semantics.
  driver.SetNumOfCycles(1);

  check_close(driver.MaxEnergy(), kMaxEnergy, "MaxEnergy from meta tree");
  check(driver.FluxParticles().size() == 3, "flavour list from meta tree");

  check(driver.GenerateNext(), "GenerateNext() first ray");
  auto const& ray = kRays.front();
  check(driver.PdgCode() == ray.pdg, "pdg code");
  check_close(driver.Weight(), 1.0, "unit-weight file stays unweighted");
  auto const& x4 = driver.Position();
  check_close(x4.X(), ray.vx * 1e-3, "vtxx passthrough (already meters)");
  check_close(x4.Z(), ray.vz * 1e-3, "vtxz passthrough (already meters)");
  auto const& p4 = driver.Momentum();
  check_close(p4.Pz(), ray.pz, "pz passthrough (GeV)");

  int generated = 1;
  while (driver.GenerateNext()) ++generated;
  check(generated == static_cast<int>(2 * kRays.size()),
        "SetNumOfCycles(1) delivers two passes (GENIE off-by-one quirk)");
  check(driver.End(), "End() true after the cycles are used up");
  check(driver.NFluxNeutrinos() == static_cast<long>(2 * kRays.size()),
        "ray count via GFluxExposureI");
  check_close(driver.GetTotalExposure(), 2.0 * kPot,
              "two passes deliver twice the file's POT (UsedPOTs)", 1e-9);
}

void test_config_validation(std::string const& flux_path) {
  std::cout << "genie_source config validation:\n";

  auto expect_throw = [](aegir::GenieSourceConfig const& cfg,
                         std::string const& needle, std::string const& what) {
    try {
      cfg.validate();
      check(false, what);
    } catch (std::runtime_error const& e) {
      check(std::string{e.what()}.find(needle) != std::string::npos,
            what + " (got: " + e.what() + ")");
    }
  };

  aegir::GenieSourceConfig cfg;
  cfg.spline_file = flux_path;  // any existing file will do here
  cfg.flux_file = flux_path;
  cfg.gdml_file = flux_path;
  cfg.validate();  // must not throw
  check(true, "complete config validates");

  auto bad = cfg;
  bad.tune.clear();
  expect_throw(bad, "tune", "empty tune rejected");

  bad = cfg;
  bad.spline_file = "/nonexistent/splines.xml";
  expect_throw(bad, "does not exist", "missing spline file rejected");

  bad = cfg;
  bad.flux_file.clear();
  expect_throw(bad, "flux_file", "missing flux_file key rejected");

  bad = cfg;
  bad.gdml_file = "/nonexistent/geometry.gdml";
  expect_throw(bad, "does not exist", "missing GDML rejected");

  bad = cfg;
  bad.seed = 0;
  expect_throw(bad, "seed", "non-positive seed rejected");

  bad = cfg;
  bad.top_volume.clear();
  expect_throw(bad, "top_volume", "empty top_volume rejected");

  bad = cfg;
  bad.max_path_lengths_file = "/nonexistent/dir/maxpl.xml";
  expect_throw(bad, "directory", "uncreatable max-path-lengths cache rejected");

  bad = cfg;
  bad.flux_format = "dk2nu";
  expect_throw(bad, "flux_format", "unknown flux_format rejected");

  bad = cfg;
  bad.flux_format = "gsimple";
  bad.flux_file = "root://eospublic.cern.ch//eos/some/remote/flux.root";
  bad.validate();  // remote URLs skip the local-existence check
  check(true, "remote flux URL accepted (existence left to the driver)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string const path =
      argc > 1 ? argv[1] : "flux_driver_test_synthetic.root";

  write_flux_file(path);
  std::cout << "wrote synthetic flux file: " << path << "\n\n";

  test_metadata_and_conversions(path);
  test_exhaustion_and_clear(path);
  test_cycling(path);
  test_flux_particles(path);
  test_bad_files();
  test_config_validation(path);

  std::string const gsimple_path = path + ".gsimple.root";
  test_gsimple_driver(gsimple_path);

  std::remove(path.c_str());
  std::remove(gsimple_path.c_str());

  if (failures) {
    std::cout << "\n" << failures << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "\nall checks passed\n";
  return 0;
}
