// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// genie_config.hpp — configuration for the GENIE driver assembly
//
// Shared by the genie_source plugin and the standalone gevgen_ship app (via
// genie_driver_setup.hpp). Kept as a plain struct with a validate() member so
// the checks are unit testable without loading the phlex plugin (see
// tests/flux_driver_test.cpp).
// Validation runs before any GENIE singleton is touched: several GENIE init
// helpers call exit() on bad input rather than throwing, so failing early
// with a clear message is the only way to surface configuration errors.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace aegir {

// Resolve a GeoModel geometry db path the same way aegir's
// geometry_geomodel_provider does: an existing path is used as-is; otherwise
// the bare filename is looked up under $SHIPGEOMETRY_ROOT/share/geometry
// (the flat install layout of the shipgeometry package).
inline std::string resolve_geometry_file(std::string const& path,
                                         std::string const& context) {
  namespace fs = std::filesystem;
  if (fs::exists(path)) return path;
  auto resolved = fs::path(path).filename();
  if (auto const* root = std::getenv("SHIPGEOMETRY_ROOT"))
    resolved = fs::path(root) / "share" / "geometry" / resolved;
  if (fs::exists(resolved)) return resolved.string();
  throw std::runtime_error(context + ": cannot locate geometry db '" + path +
                           "'; set SHIPGEOMETRY_ROOT or provide an absolute "
                           "path");
}

struct GenieSourceConfig {
  std::string tune = "G18_02a_00_000";
  std::string spline_file;  // GENIE cross-section splines (gmkspl XML output)
  std::string flux_file;    // neutrino flux file (format per flux_format)
  // 'ship': SHiP flux ntuple, schema v1 (ShipFluxDriver);
  // 'gsimple': GENIE GSimple flux (genie::flux::GSimpleNtpFlux) — the format
  // the SHiP neutrino group produces for gevgen_fnal.
  std::string flux_format = "ship";
  // Detector geometry: GeoModel SQLite db (resolved via
  // resolve_geometry_file), the same db the Geant4 side tracks through.
  std::string geometry_file;
  // Optional: restrict GENIE to the named logical volume ('cave' in
  // production); empty scans the entire world.
  std::string top_volume;
  long seed = 20260706;  // base seed; each event derives its own via Philox
  // Optional cache for the (expensive) max-path-lengths geometry scan: if the
  // file exists it is loaded, otherwise it is computed and saved there.
  std::string max_path_lengths_file;

  // `context` prefixes the error messages ("genie_source", "gevgen_ship").
  void validate(std::string const& context = "genie_source") const {
    namespace fs = std::filesystem;
    if (tune.empty())
      throw std::runtime_error(context + ": 'tune' must not be empty");
    if (seed <= 0)
      throw std::runtime_error(
          context +
          ": 'seed' must be > 0 (GENIE ignores non-positive seeds), got " +
          std::to_string(seed));
    // The per-event Philox key uses the seed as a 32-bit word (see
    // reseed_event); wider seeds would silently alias mod 2^32 while GENIE's
    // own RandGen and the GHEP metadata got the full value. (Event numbers
    // are likewise taken mod 2^32 — unreachable in practice.)
    if (seed > 4294967295L)
      throw std::runtime_error(
          context + ": 'seed' must fit in 32 bits (max 4294967295), got " +
          std::to_string(seed));
    auto require_file = [&context](std::string const& key,
                                   std::string const& path) {
      if (path.empty())
        throw std::runtime_error(context + ": config key '" + key +
                                 "' is required");
      if (!fs::exists(path))
        throw std::runtime_error(context + ": " + key + " '" + path +
                                 "' does not exist");
    };
    if (flux_format != "ship" && flux_format != "gsimple")
      throw std::runtime_error(context +
                               ": flux_format must be 'ship' or 'gsimple', "
                               "got '" +
                               flux_format + "'");
    require_file("splines", spline_file);
    if (flux_file.empty())
      throw std::runtime_error(context +
                               ": config key 'flux_file' is required");
    // Remote URLs (root://... via xrootd) cannot be checked on the local
    // filesystem; leave those to the flux driver's own error handling.
    if (!flux_file.contains("://")) require_file("flux_file", flux_file);
    if (geometry_file.empty())
      throw std::runtime_error(context +
                               ": config key 'geometry_file' is required");
    resolve_geometry_file(geometry_file, context);  // throws if unlocatable
    if (!max_path_lengths_file.empty() && !fs::exists(max_path_lengths_file)) {
      // Will be created after the geometry scan — its directory must exist.
      auto const dir = fs::path{max_path_lengths_file}.parent_path();
      if (!dir.empty() && !fs::is_directory(dir))
        throw std::runtime_error(context +
                                 ": cannot create max_path_lengths_file '" +
                                 max_path_lengths_file + "': directory '" +
                                 dir.string() + "' does not exist");
    }
  }
};

}  // namespace aegir
