// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// ship_geom_analyzer.hpp — GENIE geometry analyzer over the SHiP geometry
// service
//
// Implements genie::GeomAnalyzerI directly on the GeoModel-built Geant4
// geometry (ship::IGeometryService + ship::G4RayScanner), so GENIE sees the
// exact same geometry the Geant4 simulation tracks through — no GDML/TGeo
// conversion in between. The contract mirrors GENIE's ROOTGeomAnalyzer:
//   - path lengths are density-weighted (rho x length x element mass
//     fraction) in SI kg/m2, one entry per target nucleus;
//   - target nuclei are one averaged-A ion per element,
//     IonPdgCode(Nint(N), Z) — no isotope expansion, keeping the
//     genie-splines-ship target list valid;
//   - positions arrive and leave in SI meters (the GFluxI frame);
//   - max path lengths come from a flux-ray scan (SetScannerFlux) with a
//     safety factor, and the scan ends with flux->Clear("CycleHistory") so
//     scan rays do not count as delivered POT.
//
// All Geant4 work (GeoModel->G4 conversion, navigation, teardown) is
// confined to the process-wide ship::geometry_thread(), shared by every
// Geant4 geometry user in the process (all analyzers, and aegir's
// geant4_module in the full chain — issue #11): the conda Geant4 is an MT
// build, whose logical/physical volumes keep per-thread state usable only
// on the creating thread — and only a single thread per process may create
// geometry at all — while phlex may run even a serial source on changing
// TBB threads.

#pragma once

#include <TLorentzVector.h>
#include <TVector3.h>

#include <memory>
#include <string>

#include "Framework/EventGen/GeomAnalyzerI.h"
#include "Framework/EventGen/PathLengthList.h"
#include "Framework/ParticleData/PDGCodeList.h"

namespace genie {
class GFluxI;
}
namespace ship {
class IGeometryService;
}

namespace aegir {

class ShipGeomAnalyzer final : public genie::GeomAnalyzerI {
 public:
  enum class G4Teardown {
    // The process (e.g. aegir's geant4_module) owns Geant4 store cleanup.
    kLeaveToProcess,
    // Clean the Geant4 stores on the geometry thread in the destructor —
    // required when nothing else does it (gevgen_ship), because the store
    // singletons otherwise delete thread-local volume state from the main
    // thread at exit (aegir issue #68).
    kCleanStores,
  };

  // `top_volume` empty: scan the full world. Otherwise the named logical
  // volume becomes the scan world; it must exist and be placed exactly once,
  // and flux-frame coordinates are mapped through its placement transform.
  // Must be constructed after the GENIE tune (PDGLibrary) is initialized.
  // Shares ownership of the service (SHiPGeometryService::sharedFromFile
  // hands the same instance to every user of the same file) and holds its
  // reference for the analyzer's lifetime — GeoModel2G4 caches conversions
  // in static maps keyed by GeoModel pointers, so the tree must not be
  // freed while a later conversion in the same process is possible.
  ShipGeomAnalyzer(std::shared_ptr<ship::IGeometryService> geometry,
                   std::string const& top_volume, G4Teardown teardown);
  ~ShipGeomAnalyzer() override;

  ShipGeomAnalyzer(ShipGeomAnalyzer const&) = delete;
  ShipGeomAnalyzer& operator=(ShipGeomAnalyzer const&) = delete;
  ShipGeomAnalyzer(ShipGeomAnalyzer&&) = delete;
  ShipGeomAnalyzer& operator=(ShipGeomAnalyzer&&) = delete;

  // ROOTGeomAnalyzer-compatible knobs, used by make_genie_driver. The flux
  // scanner is the only max-path-lengths strategy: a box scanner never
  // samples beam-like axial paths and underestimates the maxima.
  void SetScannerFlux(genie::GFluxI* flux) { scanner_flux_ = flux; }
  void SetScannerNParticles(int n) { scanner_particles_ = n; }
  void SetMaxPlSafetyFactor(double sf) { safety_factor_ = sf; }
  genie::PathLengthList const& GetMaxPathLengths() const {
    return max_path_lengths_;
  }

  // Bounding box of the scan volume in the master (flux) frame, SI meters —
  // for the startup frame check against the first flux ray.
  struct Extents {
    TVector3 lo, hi;
  };
  Extents const& extents() const { return extents_; }

  // GeomAnalyzerI
  genie::PDGCodeList const& ListOfTargetNuclei() override { return targets_; }
  genie::PathLengthList const& ComputeMaxPathLengths() override;
  genie::PathLengthList const& ComputePathLengths(
      TLorentzVector const& x, TLorentzVector const& p) override;
  TVector3 const& GenerateVertex(TLorentzVector const& x,
                                 TLorentzVector const& p, int tgtpdg) override;

 private:
  struct Impl;  // all Geant4 state (used only on the geometry thread)
  std::unique_ptr<Impl> impl_;

  // Both run on the geometry thread.
  void init(std::shared_ptr<ship::IGeometryService> geometry,
            std::string const& top_volume);
  static void teardown_geant4(Impl& impl);

  genie::GFluxI* scanner_flux_ = nullptr;
  int scanner_particles_ = 10000;
  double safety_factor_ = 1.1;  // as ROOTGeomAnalyzer's default

  genie::PDGCodeList targets_;
  genie::PathLengthList path_lengths_;
  genie::PathLengthList max_path_lengths_;
  TVector3 vertex_;
  Extents extents_;
};

}  // namespace aegir
