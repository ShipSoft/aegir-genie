// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geom_analyzer_test.cpp — standalone check of ShipGeomAnalyzer
//
// Builds a small GeoModel geometry with known materials in the test (gas
// world with a copper and an iron cube on the z axis), wraps it via
// SHiPGeometryService::fromWorld, and checks the analyzer's GeomAnalyzerI
// contract against analytic values: target-nucleus list (averaged-A ion
// codes), density-weighted path lengths in SI kg/m2, vertex generation, the
// flux-driven max-path-lengths scan (including the Clear("CycleHistory")
// call and the XML cache round trip) and the top-volume frame mapping.
// Deliberately does not involve GMCJDriver, so it runs without
// cross-section splines; only the PDG library ($GENIE) is needed.
//
// Analyzers are created strictly sequentially, each with kCleanStores (the
// final second-user test emulates the process-owned cleanup instead): the
// Geant4 stores are emptied (on the shared process-wide geometry thread)
// before the next geometry is converted, so duplicate volume names never
// coexist.

#include <GeoModelKernel/GeoBox.h>
#include <GeoModelKernel/GeoElement.h>
#include <GeoModelKernel/GeoLogVol.h>
#include <GeoModelKernel/GeoMaterial.h>
#include <GeoModelKernel/GeoPhysVol.h>
#include <GeoModelKernel/GeoTransform.h>
#include <GeoModelKernel/Units.h>
#include <TLorentzVector.h>

#include <G4GeometryManager.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4PVPlacement.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4Region.hh>
#include <G4RegionStore.hh>
#include <G4SolidStore.hh>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Framework/EventGen/GFluxI.h"
#include "Framework/EventGen/PathLengthList.h"
#include "Framework/Numerical/RandomGen.h"
#include "Framework/ParticleData/PDGCodeList.h"
#include "GeometryService/GeometryThread.h"
#include "GeometryService/SHiPGeometryService.h"
#include "ship_geom_analyzer.hpp"

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

// Default tolerance 1e-6: the GeoModel -> Geant4 material conversion changes
// densities at the 1e-7 relative level (unit-constant differences).
void check_close(double a, double b, std::string const& what,
                 double tol = 1e-6) {
  check(std::abs(a - b) <= tol * std::max({1.0, std::abs(a), std::abs(b)}),
        what + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")");
}

// Geometry: 2 m gas world, 20 cm copper cube at z in [-500, -300] mm, 20 cm
// iron cube at z in [100, 300] mm (all boxes centred on the z axis).
constexpr double kWorldHalf = 1000.;     // mm
constexpr double kCubeHalf = 100.;       // mm
constexpr double kIronZ = 200.;          // mm
constexpr double kCopperZ = -400.;       // mm
constexpr double kGasDensity = 1.29e-3;  // g/cm3
constexpr double kIronDensity = 7.874;   // g/cm3
constexpr double kCopperDensity = 8.96;  // g/cm3
constexpr double kNitrogenFraction = 0.77;
constexpr double kOxygenFraction = 0.23;

// Averaged-A ion codes, as ROOTGeomAnalyzer derives them.
constexpr int kNitrogenPdg = 1000070140;  // N-14
constexpr int kOxygenPdg = 1000080160;    // O-16
constexpr int kIronPdg = 1000260560;      // Fe-56 (55.845 rounds to 56)
constexpr int kCopperPdg = 1000290640;    // Cu-64 (63.546 rounds to 64)

constexpr double kGCm3ToKgM3 = 1000.;

// `suffix` keeps volume names unique across the sequentially created
// geometries (getLogicalVolume searches the global Geant4 store by name).
PVConstLink build_world(std::string const& suffix) {
  namespace GU = GeoModelKernelUnits;
  auto* nitrogen =
      new GeoElement("Nitrogen", "N", 7, 14.007 * GU::g / GU::mole);
  auto* oxygen = new GeoElement("Oxygen", "O", 8, 15.999 * GU::g / GU::mole);
  auto* iron = new GeoElement("Iron", "Fe", 26, 55.845 * GU::g / GU::mole);
  auto* copper = new GeoElement("Copper", "Cu", 29, 63.546 * GU::g / GU::mole);

  auto* gas =
      new GeoMaterial("TestGas" + suffix, kGasDensity * GU::g / GU::cm3);
  gas->add(nitrogen, kNitrogenFraction);
  gas->add(oxygen, kOxygenFraction);
  gas->lock();
  auto* iron_mat =
      new GeoMaterial("TestIron" + suffix, kIronDensity * GU::g / GU::cm3);
  iron_mat->add(iron, 1.0);
  iron_mat->lock();
  auto* copper_mat =
      new GeoMaterial("TestCopper" + suffix, kCopperDensity * GU::g / GU::cm3);
  copper_mat->add(copper, 1.0);
  copper_mat->lock();

  auto* world = new GeoPhysVol(
      new GeoLogVol("TestWorld" + suffix,
                    new GeoBox(kWorldHalf, kWorldHalf, kWorldHalf), gas));
  world->add(new GeoTransform(GeoTrf::Translate3D(0, 0, kIronZ)));
  world->add(new GeoPhysVol(
      new GeoLogVol("TestIronBox" + suffix,
                    new GeoBox(kCubeHalf, kCubeHalf, kCubeHalf), iron_mat)));
  world->add(new GeoTransform(GeoTrf::Translate3D(0, 0, kCopperZ)));
  world->add(new GeoPhysVol(
      new GeoLogVol("TestCopperBox" + suffix,
                    new GeoBox(kCubeHalf, kCubeHalf, kCubeHalf), copper_mat)));
  return world;
}

std::unique_ptr<aegir::ShipGeomAnalyzer> make_analyzer(
    std::string const& suffix, std::string const& top_volume = {},
    aegir::ShipGeomAnalyzer::G4Teardown teardown =
        aegir::ShipGeomAnalyzer::G4Teardown::kCleanStores) {
  // Keep every GeoModel tree alive for the whole process: GeoModel2G4
  // caches conversions in static maps keyed by GeoModel pointers, and a
  // freed tree whose addresses get reused would alias a later conversion
  // onto Geant4 volumes that kCleanStores has already deleted.
  static std::vector<PVConstLink> keep_alive;
  auto world = build_world(suffix);
  keep_alive.push_back(world);
  return std::make_unique<aegir::ShipGeomAnalyzer>(
      ship::SHiPGeometryService::fromWorld(std::move(world)), top_volume,
      teardown);
}

// A ray in the flux frame: SI meters (position) and GeV (momentum), like
// GFluxI delivers them.
TLorentzVector ray_position(double z_m) { return {0., 0., z_m, 0.}; }
TLorentzVector ray_momentum() { return {0., 0., 10., 10.}; }

// Analytic density-weighted path lengths (kg/m2) for the axial ray through
// the whole world.
double const kGasPl =
    kGasDensity * kGCm3ToKgM3 * (2 * kWorldHalf - 2 * 2 * kCubeHalf) / 1e3;
double const kIronPl = kIronDensity * kGCm3ToKgM3 * 2 * kCubeHalf / 1e3;
double const kCopperPl = kCopperDensity * kGCm3ToKgM3 * 2 * kCubeHalf / 1e3;

// Minimal deterministic flux driver for the max-path-lengths scan: cycles
// through fixed rays and records Clear() calls.
class StubFlux : public genie::GFluxI {
 public:
  struct StubRay {
    TLorentzVector position;  // SI m
    TLorentzVector momentum;  // GeV
  };

  explicit StubFlux(std::vector<StubRay> rays) : rays_{std::move(rays)} {}

  genie::PDGCodeList const& FluxParticles() override { return particles_; }
  double MaxEnergy() override { return 100.; }
  bool GenerateNext() override {
    index_ = (index_ + 1) % static_cast<long>(rays_.size());
    return true;
  }
  int PdgCode() override { return 14; }
  double Weight() override { return 1.; }
  TLorentzVector const& Position() override {
    return rays_[static_cast<std::size_t>(index_)].position;
  }
  TLorentzVector const& Momentum() override {
    return rays_[static_cast<std::size_t>(index_)].momentum;
  }
  bool End() override { return false; }
  long int Index() override { return index_; }
  void Clear(Option_t* opt) override {
    clear_calls.emplace_back(opt ? opt : "");
  }
  void GenerateWeighted(bool) override {}

  std::vector<std::string> clear_calls;

 private:
  std::vector<StubRay> rays_;
  genie::PDGCodeList particles_;
  long index_ = -1;
};

void test_targets_and_path_lengths() {
  std::cout << "target list and path lengths:\n";
  auto analyzer = make_analyzer("A");

  auto const& targets = analyzer->ListOfTargetNuclei();
  check(targets.size() == 4, "four target nuclei (N, O, Fe, Cu)");
  for (int pdg : {kNitrogenPdg, kOxygenPdg, kIronPdg, kCopperPdg})
    check(std::find(targets.begin(), targets.end(), pdg) != targets.end(),
          "target list contains " + std::to_string(pdg) +
              " (averaged-A ion code)");

  auto const& extents = analyzer->extents();
  check_close(extents.lo.Z(), -1., "extents lo z (SI m)");
  check_close(extents.hi.Z(), 1., "extents hi z (SI m)");

  // Axial ray starting 0.5 m before the world.
  auto const& pl =
      analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum());
  check_close(pl.PathLength(kIronPdg), kIronPl, "iron path length (kg/m2)");
  check_close(pl.PathLength(kCopperPdg), kCopperPl, "copper path length");
  check_close(pl.PathLength(kNitrogenPdg), kGasPl * kNitrogenFraction,
              "nitrogen path length (mass-fraction weighted)");
  check_close(pl.PathLength(kOxygenPdg), kGasPl * kOxygenFraction,
              "oxygen path length");

  // A ray that misses everything: all entries present, all zero.
  auto const& miss =
      analyzer->ComputePathLengths({0., 5., -1.5, 0.}, ray_momentum());
  check(miss.size() == 4 && miss.AreAllZero(), "missing ray yields all zeros");

  // Vertex generation for the iron target: on the ray, inside the cube.
  genie::RandomGen::Instance()->SetSeed(12345);
  analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum());
  double lo = 1e9, hi = -1e9;
  bool on_axis = true;
  for (int i = 0; i < 500; ++i) {
    auto const& v =
        analyzer->GenerateVertex(ray_position(-1.5), ray_momentum(), kIronPdg);
    on_axis = on_axis && std::abs(v.X()) < 1e-9 && std::abs(v.Y()) < 1e-9;
    lo = std::min(lo, v.Z());
    hi = std::max(hi, v.Z());
  }
  check(on_axis, "vertices stay on the ray");
  check(lo >= 0.1 && hi <= 0.3, "iron vertices inside the cube [0.1, 0.3] m");
  check(hi - lo > 0.15, "vertices spread across the cube");

  // GenerateVertex without a preceding ComputePathLengths for that ray must
  // recompute rather than reuse the cached scan.
  auto const& v =
      analyzer->GenerateVertex(ray_position(-1.2), ray_momentum(), kCopperPdg);
  check(v.Z() >= -0.5 && v.Z() <= -0.3, "fresh ray places copper vertex");
}

void test_max_path_lengths() {
  std::cout << "max path lengths (flux scan):\n";
  auto analyzer = make_analyzer("B");

  // Two rays: the axial one crosses both cubes, the off-axis one crosses
  // only gas — the maxima must come from the axial ray, padded by the
  // safety factor.
  StubFlux flux{{{ray_position(-1.5), ray_momentum()},
                 {{0., 0.5, -1.5, 0.}, ray_momentum()}}};
  analyzer->SetScannerFlux(&flux);
  analyzer->SetScannerNParticles(4);  // both rays, twice

  auto const& maxpl = analyzer->ComputeMaxPathLengths();
  check_close(maxpl.PathLength(kIronPdg), 1.1 * kIronPl,
              "iron max path length = axial x 1.1 safety factor");
  check_close(maxpl.PathLength(kCopperPdg), 1.1 * kCopperPl,
              "copper max path length");
  check(
      flux.clear_calls == std::vector<std::string>{std::string{"CycleHistory"}},
      "flux Clear(\"CycleHistory\") called exactly once after the scan");
  check(&maxpl == &analyzer->GetMaxPathLengths(),
        "GetMaxPathLengths returns the scan result");

  // XML cache round trip (the GMCJDriver::UseMaxPathLengths format).
  std::string const cache = "geom_analyzer_test_maxpl.xml";
  maxpl.SaveAsXml(cache);
  genie::PathLengthList loaded;
  check(loaded.LoadFromXml(cache) == genie::kXmlOK, "XML cache loads back");
  check_close(loaded.PathLength(kIronPdg), maxpl.PathLength(kIronPdg),
              "XML round trip preserves the iron entry");
  std::remove(cache.c_str());

  // Without a scanner flux the scan must refuse to run (the box-scan
  // fallback of ROOTGeomAnalyzer is deliberately not implemented).
  auto bare = make_analyzer("C");
  try {
    bare->ComputeMaxPathLengths();
    check(false, "ComputeMaxPathLengths without flux throws");
  } catch (std::runtime_error const& e) {
    check(std::string{e.what()}.contains("SetScannerFlux"),
          "ComputeMaxPathLengths without flux throws");
  }
}

void test_top_volume() {
  std::cout << "top-volume restriction:\n";
  // Scan world = the iron cube only; rays stay in the master (flux) frame
  // and must be mapped through the cube's placement at z = +200 mm.
  auto analyzer = make_analyzer("D", "TestIronBoxD");

  auto const& targets = analyzer->ListOfTargetNuclei();
  check(targets.size() == 1 && targets[0] == kIronPdg,
        "only iron remains in the target list");

  auto const& extents = analyzer->extents();
  check_close(extents.lo.Z(), (kIronZ - kCubeHalf) / 1e3,
              "extents map the cube to master-frame z");
  check_close(extents.hi.Z(), (kIronZ + kCubeHalf) / 1e3,
              "extents hi z in the master frame");

  auto const& pl =
      analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum());
  check_close(pl.PathLength(kIronPdg), kIronPl,
              "iron path length through the off-origin top volume");

  genie::RandomGen::Instance()->SetSeed(6789);
  auto const& v =
      analyzer->GenerateVertex(ray_position(-1.5), ray_momentum(), kIronPdg);
  check(v.Z() >= 0.1 && v.Z() <= 0.3,
        "vertex lands in the cube in master-frame coordinates");

  try {
    make_analyzer("E", "NoSuchVolume");
    check(false, "unknown top_volume rejected");
  } catch (std::runtime_error const& e) {
    check(std::string{e.what()}.contains("NoSuchVolume"),
          "unknown top_volume rejected");
  }
}

void test_thread_migration() {
  std::cout << "calls from changing threads:\n";
  // phlex may run a serial source from different TBB threads; the analyzer
  // must confine Geant4 navigation to its internal thread. Exercise the
  // whole lifecycle (construction, scans, destruction) from two threads.
  std::unique_ptr<aegir::ShipGeomAnalyzer> analyzer;
  double first = 0., second = 0.;
  std::thread{[&] {
    analyzer = make_analyzer("F");
    first = analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum())
                .PathLength(kIronPdg);
  }}.join();
  std::thread{[&] {
    second = analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum())
                 .PathLength(kIronPdg);
    analyzer.reset();
  }}.join();
  check_close(first, second, "same answer from both threads");
  check_close(first, kIronPl, "and it is the analytic value");
}

void test_second_geant4_user() {
  std::cout << "second Geant4 user (geant4_module-style master init):\n";
  // In the full in-process chain, aegir's geant4_module initialises its run
  // manager on ship::geometry_thread() after the analyzer has converted the
  // shared geometry (issue #11). Emulate that master init — reopen, place a
  // world, create a region, close (voxelise) the full geometry — and check
  // the analyzer still scans correctly afterwards.
  auto analyzer = make_analyzer(
      "G", {}, aegir::ShipGeomAnalyzer::G4Teardown::kLeaveToProcess);
  double const before =
      analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum())
          .PathLength(kIronPdg);

  ship::geometry_thread().run([] {
    auto* world_lv =
        G4LogicalVolumeStore::GetInstance()->GetVolume("TestWorldG", false);
    check(world_lv != nullptr, "converted world found in the store");
    G4GeometryManager::GetInstance()->OpenGeometry();
    new G4PVPlacement(nullptr, {}, world_lv, world_lv->GetName(), nullptr,
                      false, 0);
    auto* region = new G4Region("TestWorldRegionG");
    region->AddRootLogicalVolume(world_lv);
    G4GeometryManager::GetInstance()->CloseGeometry(true, false);
  });

  // From a different thread, as phlex would call the serial source.
  double after = 0.;
  std::thread{[&] {
    after = analyzer->ComputePathLengths(ray_position(-1.5), ray_momentum())
                .PathLength(kIronPdg);
  }}.join();
  check_close(before, after, "same answer after the run-manager-style close");
  check_close(after, kIronPl, "and it is the analytic value");

  // kLeaveToProcess: the process owns the store cleanup — emulate
  // geant4_module's teardown on the geometry thread once the analyzer is
  // gone (the store singletons' at-exit destructors must find them empty,
  // aegir issue #68).
  analyzer.reset();
  ship::geometry_thread().run([] {
    G4GeometryManager::GetInstance()->OpenGeometry();
    G4RegionStore::Clean();
    G4PhysicalVolumeStore::Clean();
    G4LogicalVolumeStore::Clean();
    G4SolidStore::Clean();
  });
}

}  // namespace

int main() {
  test_targets_and_path_lengths();
  test_max_path_lengths();
  test_top_volume();
  test_thread_migration();
  test_second_geant4_user();

  if (failures) {
    std::cout << failures << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
