// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "genie_driver_setup.hpp"

#include <TGeoManager.h>

#include <filesystem>
#include <stdexcept>

#include "Framework/Conventions/Units.h"
#include "Framework/EventGen/GMCJDriver.h"
#include "Framework/Numerical/RandomGen.h"
#include "Framework/Utils/AppInit.h"
#include "Framework/Utils/RunOpt.h"
#include "Tools/Geometry/ROOTGeomAnalyzer.h"
#include "philox_rng.hpp"
#include "ship_flux_driver.hpp"

namespace aegir {

namespace {

// Philox stream selector for the GENIE reseeding, distinct from the other
// aegir generators (see philox_rng.hpp).
constexpr std::uint32_t kGenieStream = 0x47454E49;  // "GENI"

}  // namespace

GenieDriverBundle::GenieDriverBundle() = default;
GenieDriverBundle::GenieDriverBundle(GenieDriverBundle&&) noexcept = default;
GenieDriverBundle& GenieDriverBundle::operator=(GenieDriverBundle&&) noexcept =
    default;
GenieDriverBundle::~GenieDriverBundle() = default;

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

  // 6. Flux: cycle so KeepOnThrowingFluxNeutrinos always finds a ray; the
  //    delivered POT is still accounted through GFluxExposureI.
  bundle.flux = std::make_unique<ShipFluxDriver>(cfg.flux_file,
                                                 /*cycle=*/true);

  // 7. MC job driver.
  bundle.driver = std::make_unique<genie::GMCJDriver>();
  bundle.driver->UseFluxDriver(bundle.flux.get());
  bundle.driver->UseGeomAnalyzer(bundle.geom.get());
  bundle.driver->UseSplines();
  bundle.driver->ForceSingleProbScale();          // unweighted events
  bundle.driver->KeepOnThrowingFluxNeutrinos(true);  // one interaction/event

  // The max-path-lengths geometry scan is expensive; cache it as XML when
  // the config names a file.
  bool const use_cached = !cfg.max_path_lengths_file.empty() &&
                          std::filesystem::exists(cfg.max_path_lengths_file);
  if (use_cached &&
      !bundle.driver->UseMaxPathLengths(cfg.max_path_lengths_file))
    throw std::runtime_error(context +
                             ": failed to load max path lengths from '" +
                             cfg.max_path_lengths_file +
                             "' — delete the file to recompute it");

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
  genie::RandomGen::Instance()->SetSeed(seed);
}

}  // namespace aegir
