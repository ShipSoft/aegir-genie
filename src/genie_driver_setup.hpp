// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: GPL-3.0-or-later

// genie_driver_setup.hpp — shared GENIE MC-job assembly
//
// One code path builds the GMCJDriver machinery (tune, splines, TGeo
// geometry, SHiP flux driver) for both consumers, so their events are
// directly comparable:
//   - the phlex source plugin (genie_source.cpp), and
//   - the standalone generator app (gevgen_ship.cpp).
//
// Initialization order matters (cf. nugen/GENIEHelper): the tune must be
// set and built before anything touches PDGLibrary; splines load after the
// tune; the geometry analyzer and flux driver attach to the GMCJDriver last.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "genie_config.hpp"

namespace genie {
class GMCJDriver;
namespace geometry {
class ROOTGeomAnalyzer;
}
}  // namespace genie

namespace aegir {

class ShipFluxDriver;

// The assembled MC-job machinery. GMCJDriver only borrows the flux driver
// and geometry analyzer, so the bundle owns all three; keep it alive for as
// long as events are generated.
struct GenieDriverBundle {
  std::unique_ptr<genie::geometry::ROOTGeomAnalyzer> geom;
  std::unique_ptr<ShipFluxDriver> flux;
  std::unique_ptr<genie::GMCJDriver> driver;

  // Out of line: the members' types are incomplete here.
  GenieDriverBundle();
  GenieDriverBundle(GenieDriverBundle&&) noexcept;
  GenieDriverBundle& operator=(GenieDriverBundle&&) noexcept;
  ~GenieDriverBundle();
};

// Validates the config and assembles a fully Configure()d GMCJDriver:
// tune -> Messenger -> RandGen -> splines -> GDML/ROOTGeomAnalyzer -> cycling
// ShipFluxDriver -> GMCJDriver (UseSplines, ForceSingleProbScale,
// KeepOnThrowingFluxNeutrinos), with optional XML caching of the expensive
// max-path-lengths geometry scan. `context` prefixes error messages (e.g.
// "genie_source", "gevgen_ship").
//
// GENIE is a web of unsynchronized singletons: call this once per process.
GenieDriverBundle make_genie_driver(GenieSourceConfig const& cfg,
                                    std::string const& context);

// Reseed GENIE for one event: derives a per-event seed from (base seed,
// event number) with Philox, so re-running the same event yields the same
// draws regardless of what was generated before — and the plugin and the
// standalone app produce identical sequences for identical configs.
// RandomGen::SetSeed also reseeds ROOT's global gRandom.
void reseed_event(long base_seed, std::uint32_t event_number);

}  // namespace aegir
