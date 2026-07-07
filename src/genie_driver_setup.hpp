// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

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
class EventRecord;
class GFluxI;
class GMCJDriver;
namespace flux {
class GFluxExposureI;
}
namespace geometry {
class ROOTGeomAnalyzer;
}
}  // namespace genie

namespace aegir {

// The assembled MC-job machinery. GMCJDriver only borrows the flux driver
// and geometry analyzer, so the bundle owns all three; keep it alive for as
// long as events are generated. The flux driver is either an
// aegir::ShipFluxDriver (flux_format 'ship') or a
// genie::flux::GSimpleNtpFlux ('gsimple'); consumers should use the GFluxI /
// GFluxExposureI interfaces where possible.
struct GenieDriverBundle {
  std::unique_ptr<genie::geometry::ROOTGeomAnalyzer> geom;
  std::unique_ptr<genie::GFluxI> flux;
  std::unique_ptr<genie::GMCJDriver> driver;

  // Exposure/POT accounting view of the flux driver (both implementations
  // provide it); nullptr only if a future driver does not.
  genie::flux::GFluxExposureI* exposure() const;

  // Out of line: the members' types are incomplete here.
  GenieDriverBundle();
  GenieDriverBundle(GenieDriverBundle&&) noexcept;
  GenieDriverBundle& operator=(GenieDriverBundle&&) noexcept;
  ~GenieDriverBundle();
};

// Validates the config and assembles a fully Configure()d GMCJDriver:
// tune -> Messenger -> RandGen -> splines -> GDML/ROOTGeomAnalyzer -> cycling
// flux driver -> GMCJDriver (UseSplines, ForceSingleProbScale,
// KeepOnThrowingFluxNeutrinos), with optional XML caching of the expensive
// max-path-lengths geometry scan. Logs the first flux ray and the geometry
// bounding box at startup so coordinate-frame mismatches are visible
// immediately. `context` prefixes error messages (e.g. "genie_source",
// "gevgen_ship").
//
// GENIE is a web of unsynchronized singletons: call this once per process.
GenieDriverBundle make_genie_driver(GenieSourceConfig const& cfg,
                                    std::string const& context);

// Reseed every RNG stream GENIE draws from for one event — GENIE's
// RandomGen (TRandom3), ROOT's global gRandom, and Pythia6's internal
// RANMAR generator — with Philox-derived seeds from (base seed, event
// number). Together with in-order flux consumption this makes each event a
// pure function of (config, base seed, event number), so the plugin and the
// standalone app produce identical sequences for identical configs, and
// re-running a job reproduces it exactly.
void reseed_event(long base_seed, std::uint32_t event_number);

// Debug aid: when the AEGIR_GENIE_RNG_TRACE environment variable is set,
// reseed_event and trace_event print per-event RNG/flux/vertex state to
// stderr, so the plugin and app paths can be diffed line by line.
void trace_event(std::uint32_t event_number, genie::EventRecord const& event,
                 genie::GFluxI& flux);

}  // namespace aegir
