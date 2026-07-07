// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "genie_driver_setup.hpp"

#include <TChain.h>
#include <TGeoBBox.h>
#include <TGeoManager.h>
#include <TGeoVolume.h>
#include <TPythia6.h>
#include <TRandom3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <thread>

#include "Framework/Conventions/Units.h"
#include "Framework/EventGen/EventRecord.h"
#include "Framework/EventGen/GMCJDriver.h"
#include "Framework/GHEP/GHepParticle.h"
#include "Framework/Numerical/RandomGen.h"
#include "Framework/Utils/AppInit.h"
#include "Framework/Utils/RunOpt.h"
#include "Tools/Flux/GFluxExposureI.h"
#include "Tools/Flux/GSimpleNtpFlux.h"
#include "Tools/Geometry/ROOTGeomAnalyzer.h"
#include "philox_rng.hpp"
#include "ship_flux_driver.hpp"

namespace aegir {

namespace {

// Philox stream selector for the GENIE reseeding, distinct from the other
// aegir generators (see philox_rng.hpp).
constexpr std::uint32_t kGenieStream = 0x47454E49;  // "GENI"

// Startup sanity log: the first flux ray (SI meters, per the GFluxI
// contract) next to the geometry bounding box (geometry units, cm for
// ROOT/GDML), so coordinate-frame or unit mismatches between flux file and
// geometry are visible immediately.
void log_frame_check(GenieSourceConfig const& cfg, TGeoManager const& geo,
                     genie::GFluxI& flux) {
  double x = 0, y = 0, z = 0, e = 0;
  int pdg = 0;
  if (auto* ship = dynamic_cast<aegir::ShipFluxDriver*>(&flux)) {
    auto const first = ship->peek_first_ray();
    x = first.position.X();
    y = first.position.Y();
    z = first.position.Z();
    e = first.momentum.E();
    pdg = first.pdg;
  } else if (auto* gs = dynamic_cast<genie::flux::GSimpleNtpFlux*>(&flux)) {
    // Reading the chain fills the driver's current-entry buffer; harmless,
    // because the driver re-reads whatever entry it needs on GenerateNext.
    gs->GetFluxTChain()->GetEntry(0);
    auto const* entry = gs->GetCurrentEntry();
    x = entry->vtxx;
    y = entry->vtxy;
    z = entry->vtxz;
    e = entry->E;
    pdg = entry->pdg;
  }
  auto const* top = geo.GetTopVolume();
  auto const* box = dynamic_cast<TGeoBBox const*>(top->GetShape());
  double const dx = box ? box->GetDX() : -1;
  double const dy = box ? box->GetDY() : -1;
  double const dz = box ? box->GetDZ() : -1;
  std::fprintf(
      stderr,
      "[genie-driver] frame check: first flux ray (%s) pdg=%d E=%g GeV at "
      "(%g, %g, %g) m; geometry top volume '%s' half-lengths (%g, %g, %g) cm "
      "= (%g, %g, %g) m\n",
      cfg.flux_format.c_str(), pdg, e, x, y, z, top->GetName(), dx, dy, dz,
      dx / 100., dy / 100., dz / 100.);
}

}  // namespace

GenieDriverBundle::GenieDriverBundle() = default;
GenieDriverBundle::GenieDriverBundle(GenieDriverBundle&&) noexcept = default;
GenieDriverBundle& GenieDriverBundle::operator=(GenieDriverBundle&&) noexcept =
    default;
GenieDriverBundle::~GenieDriverBundle() = default;

genie::flux::GFluxExposureI* GenieDriverBundle::exposure() const {
  return dynamic_cast<genie::flux::GFluxExposureI*>(flux.get());
}

GenieDriverBundle make_genie_driver(GenieSourceConfig const& cfg,
                                    std::string const& context) {
  cfg.validate(context);

  // 1. Tune selection — before any access to PDGLibrary or AlgFactory.
  auto* run_opt = genie::RunOpt::Instance();
  run_opt->SetTuneName(cfg.tune);
  if (!run_opt->Tune())
    throw std::runtime_error(context + ": tune '" + cfg.tune +
                             "' not recognised by GENIE");
  run_opt->BuildTune();

  // 2. Tame GENIE's logging (ships with the GENIE config files; resolved
  //    via $GENIE/config).
  genie::utils::app_init::MesgThresholds("Messenger_whisper.xml");

  // 3. Base RNG seed; callers re-seed per event via reseed_event().
  genie::utils::app_init::RandGen(cfg.seed);

  // 4. Cross-section splines. XSecTable exit()s the process when the file
  //    is unusable, so cfg.validate() checked its existence up front.
  genie::utils::app_init::XSecTable(cfg.spline_file, true);

  GenieDriverBundle bundle;

  // 5. Geometry: the same GDML the Geant4 side tracks through, imported
  //    into TGeo (lengths in cm, ROOT's native unit).
  auto* geo = TGeoManager::Import(cfg.gdml_file.c_str());
  if (!geo)
    throw std::runtime_error(context + ": TGeoManager::Import failed on '" +
                             cfg.gdml_file + "'");
  bundle.geom = std::make_unique<genie::geometry::ROOTGeomAnalyzer>(geo);
  bundle.geom->SetLengthUnits(genie::units::centimeter);
  bundle.geom->SetDensityUnits(genie::units::gram_centimeter3);
  bundle.geom->SetTopVolName(cfg.top_volume);

  // 6. Flux. Both drivers cycle through the file indefinitely so
  //    KeepOnThrowingFluxNeutrinos always finds a ray; delivered POT is
  //    accounted through GFluxExposureI either way.
  if (cfg.flux_format == "gsimple") {
    // GENIE's own GSimple flux driver — the format the SHiP neutrino group
    // produces for gevgen_fnal. Entry positions are lab-frame meters (SI,
    // matching the GFluxI contract), momenta GeV; we assume the SHiP global
    // frame, i.e. the same frame as the GDML geometry.
    //
    // Weighted rays: by default the driver unweights internally
    // (GenerateNext accept–rejects on wgt/maxWgt, drawing from
    // RandomGen::RndFlux — covered by reseed_event — and then reports
    // Weight() = 1), exactly as in gevgen_fnal, which never calls
    // SetGenWeighted. Combined with GMCJDriver::ForceSingleProbScale the
    // generated events are unweighted.
    auto gsimple = std::make_unique<genie::flux::GSimpleNtpFlux>();
    // The second argument is a config string for this driver (not a beam
    // location): "no-offset-index" starts at entry 0 instead of a
    // RandomGen-chosen random offset, keeping runs trivially reproducible
    // and file consumption aligned between the plugin and gevgen_ship.
    gsimple->LoadBeamSimData(cfg.flux_file, "no-offset-index");
    if (gsimple->End())
      throw std::runtime_error(context +
                               ": GSimpleNtpFlux loaded no flux "
                               "entries from '" +
                               cfg.flux_file + "'");
    gsimple->SetNumOfCycles(0);  // 0 = recycle indefinitely (as gevgen_fnal)
    bundle.flux = std::move(gsimple);
  } else {
    bundle.flux = std::make_unique<ShipFluxDriver>(cfg.flux_file,
                                                   /*cycle=*/true);
  }

  log_frame_check(cfg, *geo, *bundle.flux);

  // Max-path-length scan strategy: scan with the actual flux, as gevgen_fnal
  // does by default. The alternative box scanner throws random-direction
  // rays from the bounding-box faces and never samples beam-like axial paths
  // through long, thin volumes, so it underestimates the maximum
  // density-weighted path lengths for a forward beam — GMCJDriver's
  // probability normalisation then fails mid-run ("negative no-interaction
  // probability", followed by exit(1)). The scan consumes flux rays (10000
  // by default) before event generation; ComputeMaxPathLengths resets the
  // exposure history afterwards via Clear("CycleHistory"), so the scan does
  // not count towards the delivered POT. Consequently a cached
  // max_path_lengths_file depends on the flux as well as the geometry —
  // regenerate it when either changes.
  bundle.geom->SetScannerFlux(bundle.flux.get());

  // 7. MC job driver.
  bundle.driver = std::make_unique<genie::GMCJDriver>();
  bundle.driver->UseFluxDriver(bundle.flux.get());
  bundle.driver->UseGeomAnalyzer(bundle.geom.get());
  bundle.driver->UseSplines();
  bundle.driver->ForceSingleProbScale();             // unweighted events
  bundle.driver->KeepOnThrowingFluxNeutrinos(true);  // one interaction/event

  // The max-path-lengths geometry scan is expensive; cache it as XML when
  // the config names a file.
  bool const use_cached = !cfg.max_path_lengths_file.empty() &&
                          std::filesystem::exists(cfg.max_path_lengths_file);
  if (use_cached &&
      !bundle.driver->UseMaxPathLengths(cfg.max_path_lengths_file))
    throw std::runtime_error(
        context + ": failed to load max path lengths from '" +
        cfg.max_path_lengths_file + "' — delete the file to recompute it");

  bundle.driver->Configure();

  if (!cfg.max_path_lengths_file.empty() && !use_cached)
    bundle.geom->GetMaxPathLengths().SaveAsXml(cfg.max_path_lengths_file);

  return bundle;
}

void reseed_event(long base_seed, std::uint32_t event_number) {
  PhiloxRng rng{static_cast<std::uint32_t>(base_seed) ^ event_number,
                kGenieStream};
  // TRandom3 seeds are UInt_t; keep the full 32-bit range but avoid 0
  // (which TRandom3 interprets as "seed from clock").
  auto const seed = 1 + static_cast<long>(rng.uniform() * 4294967294.0);
  // Reseeds GENIE's TRandom3 *and* ROOT's global gRandom (a plain global,
  // not thread-local — verified for the conda ROOT build).
  genie::RandomGen::Instance()->SetSeed(seed);
  // Pythia6's hadronization draws come from its own Fortran RANMAR generator
  // (pyr_ in libPythia6, MRPY common block) — a hidden RNG stream that
  // RandomGen::SetSeed does not touch and whose state otherwise carries
  // across events. Reseed it too (MRPY(1) must be < 900000000; MRPY(2) = 0
  // forces re-initialization on the next PYR call), so a GENIE event is a
  // pure function of (base seed, event number, flux position).
  auto* py6 = TPythia6::Instance();
  py6->SetMRPY(1, 1 + static_cast<int>(rng.uniform() * 899999998.0));
  py6->SetMRPY(2, 0);
  if (std::getenv("AEGIR_GENIE_RNG_TRACE")) {
    std::fprintf(stderr,
                 "[rng-trace] reseed event=%u base_seed=%ld seed=%ld "
                 "thread=%zu gRandom=%p gRandom_first=%.17g\n",
                 event_number, base_seed, seed,
                 std::hash<std::thread::id>{}(std::this_thread::get_id()),
                 static_cast<void*>(gRandom), [] {
                   TRandom3 probe;
                   probe.SetSeed(gRandom->GetSeed());
                   return probe.Rndm();
                 }());
  }
}

void trace_event(std::uint32_t event_number, genie::EventRecord const& event,
                 genie::GFluxI& flux) {
  if (!std::getenv("AEGIR_GENIE_RNG_TRACE")) return;
  auto const* vtx = event.Vertex();
  auto const* exposure = dynamic_cast<genie::flux::GFluxExposureI*>(&flux);
  std::fprintf(stderr,
               "[rng-trace] event=%u flux_index=%ld rays_used=%ld "
               "vtx=(%.17g, %.17g, %.17g) n_entries=%d probe_pdg=%d\n",
               event_number, flux.Index(),
               exposure ? exposure->NFluxNeutrinos() : -1, vtx->X(), vtx->Y(),
               vtx->Z(), event.GetEntries(),
               event.Particle(0) ? event.Particle(0)->Pdg() : 0);
}

}  // namespace aegir
