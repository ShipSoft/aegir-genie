// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: GPL-3.0-or-later

// flux_driver_test.cpp — standalone check of ShipFluxDriver and the
// genie_source config validation
//
// Writes a synthetic schema-v1 neutrino flux file with known values, then
// drives ShipFluxDriver::GenerateNext() through it and checks the unit
// conversions, exhaustion/cycling behaviour and exposure accounting.
// Deliberately does not involve GMCJDriver, so it runs without cross-section
// splines (which do not exist yet as a package); everything here works today.

#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleWriter.hxx>
#include <TFile.h>
#include <TSystem.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

  driver.Clear("");
  check(!driver.End(), "Clear() rewinds");
  check(driver.GenerateNext(), "GenerateNext() works again after Clear()");
  check(driver.Index() == 0, "rewound to first entry");
  check(driver.NFluxNeutrinos() == static_cast<long>(kRays.size()) + 1,
        "exposure keeps accumulating across Clear()");
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
  bad.max_path_lengths_file = "/nonexistent/dir/maxpl.xml";
  expect_throw(bad, "directory", "uncreatable max-path-lengths cache rejected");
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

  std::remove(path.c_str());

  if (failures) {
    std::cout << "\n" << failures << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "\nall checks passed\n";
  return 0;
}
