// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ship_geom_analyzer.hpp"

#include <CLHEP/Units/SystemOfUnits.h>
#include <TMath.h>

#include <G4Element.hh>
#include <G4GeometryManager.hh>
#include <G4LogicalVolume.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4Material.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4RegionStore.hh>
#include <G4RotationMatrix.hh>
#include <G4SolidStore.hh>
#include <G4ThreeVector.hh>
#include <G4VPhysicalVolume.hh>
#include <G4VSolid.hh>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Framework/EventGen/GFluxI.h"
#include "Framework/Numerical/RandomGen.h"
#include "Framework/ParticleData/PDGUtils.h"
#include "GeometryService/G4RayScanner.h"
#include "GeometryService/GeometryThread.h"
#include "GeometryService/IGeometryService.h"

namespace aegir {

namespace {

// The process-wide Geant4 geometry thread (shared with every other Geant4
// geometry user, e.g. aegir's geant4_module in the full chain — issue #11):
// only a single thread per process may create Geant4 geometry.
using ship::geometry_thread;

// One target nucleus per element, averaged-A — exactly what ROOTGeomAnalyzer
// derives from a TGeo mixture element, so the genie-splines-ship target list
// stays valid (natural Cu -> Cu-64, Ni -> Ni-59, ...).
int target_pdg(G4Element const& element) {
  return genie::pdg::IonPdgCode(TMath::Nint(element.GetN()),
                                element.GetZasInt());
}

}  // namespace

struct ShipGeomAnalyzer::Impl {
  // Density x mass fraction per target nucleus, in SI kg/m3 — multiplied by
  // the traversal length in m this yields the kg/m2 path lengths GENIE
  // expects from an SI-configured analyzer.
  using TargetWeights = std::vector<std::pair<int, double>>;

  G4Teardown teardown;
  // Shared with any other user of the same geometry file (aegir's geometry
  // provider in the full phlex chain gets the same instance via
  // SHiPGeometryService::sharedFromFile) and held for the analyzer's
  // lifetime: GeoModel2G4's Geo2G4LVFactory caches converted volumes in
  // function-static maps keyed by GeoModel *pointers*, so freeing the
  // GeoModel tree lets a later conversion in the same process hit stale
  // cache entries when allocations reuse the freed addresses.
  std::shared_ptr<ship::IGeometryService> geometry;
  std::unique_ptr<ship::G4RayScanner> scanner;
  std::map<G4Material const*, TargetWeights> weights;

  // Placement of the scan volume in the master (flux) frame:
  // p_master = rotation * p_top + translation.
  G4RotationMatrix rotation;  // identity when scanning the full world
  G4ThreeVector translation{0., 0., 0.};

  // Last scanned ray, reused by GenerateVertex (GMCJDriver calls it right
  // after ComputePathLengths with the same arguments).
  TLorentzVector last_x, last_p;
  bool have_scan = false;
  ship::RayScan scan;            // segments in mm, top frame
  G4ThreeVector scan_origin;     // ray origin, mm, top frame
  G4ThreeVector scan_direction;  // unit vector, top frame

  G4ThreeVector master_to_top(G4ThreeVector const& p) const {
    return rotation.inverse() * (p - translation);
  }
  G4ThreeVector top_to_master(G4ThreeVector const& p) const {
    return rotation * p + translation;
  }
};

namespace {

// Depth-first search for placements of `target` under `mother`, accumulating
// the mother-frame->master-frame transform. Appends one (rotation,
// translation) per placement found.
void find_placements(
    G4LogicalVolume const* mother, G4LogicalVolume const* target,
    G4RotationMatrix const& rotation, G4ThreeVector const& translation,
    std::vector<std::pair<G4RotationMatrix, G4ThreeVector>>& found) {
  for (std::size_t i = 0; i < mother->GetNoDaughters(); ++i) {
    auto const* pv = mother->GetDaughter(i);
    auto const object_rotation = pv->GetObjectRotationValue();
    auto const daughter_rotation = rotation * object_rotation;
    auto const daughter_translation =
        rotation * pv->GetObjectTranslation() + translation;
    auto const* lv = pv->GetLogicalVolume();
    if (lv == target)
      found.emplace_back(daughter_rotation, daughter_translation);
    find_placements(lv, target, daughter_rotation, daughter_translation, found);
  }
}

// All materials used in `lv`'s subtree, including `lv` itself.
void collect_materials(G4LogicalVolume const* lv,
                       std::set<G4Material const*>& materials) {
  materials.insert(lv->GetMaterial());
  for (std::size_t i = 0; i < lv->GetNoDaughters(); ++i)
    collect_materials(lv->GetDaughter(i)->GetLogicalVolume(), materials);
}

}  // namespace

// Runs on the geometry thread; shared by the destructor and the
// constructor's failure path (a throw during geometry validation must not
// leave live Geant4 state behind either).
void ShipGeomAnalyzer::teardown_geant4(Impl& impl) {
  impl.scanner.reset();
  if (impl.teardown == G4Teardown::kCleanStores) {
    // Nothing else in this process tears Geant4 down: empty the stores on
    // the thread that owns the volumes' thread-local state, so the store
    // singletons' at-exit destructors have nothing left to delete from
    // the wrong thread (aegir issue #68).
    G4GeometryManager::GetInstance()->OpenGeometry();
    G4RegionStore::Clean();
    G4PhysicalVolumeStore::Clean();
    G4LogicalVolumeStore::Clean();
    G4SolidStore::Clean();
  }
  impl.geometry.reset();
}

ShipGeomAnalyzer::ShipGeomAnalyzer(
    std::shared_ptr<ship::IGeometryService> geometry,
    std::string const& top_volume, G4Teardown teardown)
    : impl_{std::make_unique<Impl>()} {
  impl_->teardown = teardown;

  geometry_thread().run([&] {
    try {
      init(std::move(geometry), top_volume);
    } catch (...) {
      // geant4WorldLogical() converts before the validation that can throw;
      // don't leave the converted volumes behind on failure.
      teardown_geant4(*impl_);
      throw;
    }
  });

  path_lengths_ = genie::PathLengthList{targets_};
  max_path_lengths_ = genie::PathLengthList{targets_};
}

// Geometry-thread half of the constructor.
void ShipGeomAnalyzer::init(std::shared_ptr<ship::IGeometryService> geometry,
                            std::string const& top_volume) {
  {
    auto* world = geometry->geant4WorldLogical();
    auto* top = world;
    if (!top_volume.empty()) {
      top = geometry->getLogicalVolume(top_volume);
      if (!top)
        throw std::runtime_error(
            "ShipGeomAnalyzer: top_volume '" + top_volume +
            "' not found in the converted Geant4 geometry");
      if (top != world) {
        std::vector<std::pair<G4RotationMatrix, G4ThreeVector>> placements;
        find_placements(world, top, G4RotationMatrix{}, G4ThreeVector{},
                        placements);
        if (placements.empty())
          throw std::runtime_error("ShipGeomAnalyzer: top_volume '" +
                                   top_volume + "' is not placed in the world");
        if (placements.size() > 1)
          throw std::runtime_error(
              "ShipGeomAnalyzer: top_volume '" + top_volume + "' is placed " +
              std::to_string(placements.size()) +
              " times — the flux-frame mapping would be ambiguous");
        impl_->rotation = placements.front().first;
        impl_->translation = placements.front().second;
      }
    }
    impl_->geometry = std::move(geometry);  // see Impl::geometry
    impl_->scanner = std::make_unique<ship::G4RayScanner>(top);

    // Material -> per-target SI weights, and the target-nucleus list.
    std::set<G4Material const*> materials;
    collect_materials(top, materials);
    std::set<int> target_set;
    for (auto const* material : materials) {
      double const density =
          material->GetDensity() / (CLHEP::kg / CLHEP::m3);  // -> kg/m3
      auto const& elements = *material->GetElementVector();
      auto const* fractions = material->GetFractionVector();  // mass fractions
      Impl::TargetWeights material_weights;
      for (std::size_t i = 0; i < elements.size(); ++i) {
        int const pdg = target_pdg(*elements[i]);
        material_weights.emplace_back(pdg, density * fractions[i]);
        target_set.insert(pdg);
      }
      impl_->weights.emplace(material, std::move(material_weights));
    }
    for (int pdg : target_set) targets_.push_back(pdg);

    // Scan-volume bounding box, mapped to the master frame.
    G4ThreeVector lo, hi;
    top->GetSolid()->BoundingLimits(lo, hi);
    TVector3 min_m{INFINITY, INFINITY, INFINITY};
    TVector3 max_m{-INFINITY, -INFINITY, -INFINITY};
    for (int corner = 0; corner < 8; ++corner) {
      G4ThreeVector const local{corner & 1 ? hi.x() : lo.x(),
                                corner & 2 ? hi.y() : lo.y(),
                                corner & 4 ? hi.z() : lo.z()};
      auto const master = impl_->top_to_master(local) / 1e3;  // mm -> m
      for (int axis = 0; axis < 3; ++axis) {
        min_m[axis] = std::min(min_m[axis], master[axis]);
        max_m[axis] = std::max(max_m[axis], master[axis]);
      }
    }
    extents_ = Extents{min_m, max_m};
  }
}

ShipGeomAnalyzer::~ShipGeomAnalyzer() {
  geometry_thread().run([this] { teardown_geant4(*impl_); });
}

genie::PathLengthList const& ShipGeomAnalyzer::ComputePathLengths(
    TLorentzVector const& x, TLorentzVector const& p) {
  // Flux frame: SI meters (GFluxI contract) -> mm -> top-volume frame.
  G4ThreeVector const origin_master{x.X() * 1e3, x.Y() * 1e3, x.Z() * 1e3};
  G4ThreeVector const direction_master =
      G4ThreeVector{p.Px(), p.Py(), p.Pz()}.unit();
  auto const origin = impl_->master_to_top(origin_master);
  auto const direction = (impl_->rotation.inverse() * direction_master).unit();

  impl_->scan = geometry_thread().run(
      [&] { return impl_->scanner->scan(origin, direction); });
  impl_->scan_origin = origin;
  impl_->scan_direction = direction;
  impl_->last_x = x;
  impl_->last_p = p;
  impl_->have_scan = true;

  path_lengths_.SetAllToZero();
  for (auto const& segment : impl_->scan.segments) {
    double const length_m = segment.length / 1e3;  // mm -> m
    for (auto const& [pdg, weight] : impl_->weights.at(segment.material))
      path_lengths_.AddPathLength(pdg, weight * length_m);  // kg/m2
  }
  return path_lengths_;
}

genie::PathLengthList const& ShipGeomAnalyzer::ComputeMaxPathLengths() {
  if (!scanner_flux_)
    throw std::runtime_error(
        "ShipGeomAnalyzer: SetScannerFlux must be called before "
        "ComputeMaxPathLengths — the flux scan is the only supported "
        "max-path-lengths strategy");
  max_path_lengths_.SetAllToZero();
  {
    // The scan consumes flux rays; reset the exposure bookkeeping on every
    // exit — also when a scan throws — so they never count towards the
    // delivered POT (as ROOTGeomAnalyzer does).
    struct ClearCycleHistory {
      genie::GFluxI* flux;
      ~ClearCycleHistory() { flux->Clear("CycleHistory"); }
    } const guard{scanner_flux_};
    for (int i = 0; i < scanner_particles_; ++i) {
      if (!scanner_flux_->GenerateNext()) continue;
      auto const& pl = ComputePathLengths(scanner_flux_->Position(),
                                          scanner_flux_->Momentum());
      for (auto const& [pdg, length] : pl) {
        double const padded = length * safety_factor_;
        if (padded > max_path_lengths_.PathLength(pdg))
          max_path_lengths_.SetPathLength(pdg, padded);
      }
    }
  }
  return max_path_lengths_;
}

TVector3 const& ShipGeomAnalyzer::GenerateVertex(TLorentzVector const& x,
                                                 TLorentzVector const& p,
                                                 int tgtpdg) {
  if (!impl_->have_scan || impl_->last_x != x || impl_->last_p != p)
    ComputePathLengths(x, p);

  // Weight per segment for this target: 0 in materials that do not contain
  // it. Units cancel in the ratio, so plain mm x (kg/m3) suffices here.
  auto segment_weight = [this, tgtpdg](ship::RaySegment const& segment) {
    double weight = 0.;
    for (auto const& [pdg, w] : impl_->weights.at(segment.material))
      if (pdg == tgtpdg) weight += w;
    return weight * segment.length;
  };

  double total = 0.;
  for (auto const& segment : impl_->scan.segments)
    total += segment_weight(segment);
  if (total <= 0.)
    throw std::runtime_error(
        "ShipGeomAnalyzer: GenerateVertex called for target " +
        std::to_string(tgtpdg) +
        " with zero path length on the current ray — GMCJDriver should only "
        "request targets seen by ComputePathLengths");

  // Uniform in the density-weighted path of this target along the ray
  // (mirrors ROOTGeomAnalyzer, including the RndGeom stream).
  double const r = genie::RandomGen::Instance()->RndGeom().Rndm() * total;
  double distance = impl_->scan.entry;  // mm from the ray origin
  double accumulated = 0.;
  double last_target_end = distance;  // end of the last target-bearing segment
  bool placed = false;
  for (auto const& segment : impl_->scan.segments) {
    double const weight = segment_weight(segment);
    if (weight > 0. && accumulated + weight >= r) {
      distance += segment.length * ((r - accumulated) / weight);
      placed = true;
      break;
    }
    accumulated += weight;
    distance += segment.length;
    if (weight > 0.) last_target_end = distance;
  }
  if (!placed)  // floating-point edge: r landed just beyond the accumulation
    distance = last_target_end;

  auto const vertex_top = impl_->scan_origin + distance * impl_->scan_direction;
  auto const vertex_master = impl_->top_to_master(vertex_top);
  vertex_.SetXYZ(vertex_master.x() / 1e3, vertex_master.y() / 1e3,
                 vertex_master.z() / 1e3);  // mm -> m (SI)
  return vertex_;
}

}  // namespace aegir
