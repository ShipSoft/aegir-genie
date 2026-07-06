// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: GPL-3.0-or-later

// genie_config.hpp — configuration for the embedded GENIE source
//
// Kept as a plain struct with a validate() member so the checks are unit
// testable without loading the phlex plugin (see tests/flux_driver_test.cpp).
// Validation runs before any GENIE singleton is touched: several GENIE init
// helpers call exit() on bad input rather than throwing, so failing early
// with a clear message is the only way to surface configuration errors.

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace aegir {

struct GenieSourceConfig {
  std::string tune = "G18_02a_00_000";
  std::string spline_file;  // GENIE cross-section splines (gmkspl XML output)
  std::string flux_file;    // SHiP neutrino flux ntuple (schema v1)
  std::string gdml_file;    // detector geometry for TGeoManager::Import
  std::string top_volume = "World";
  long seed = 20260706;  // base seed; each event derives its own via Philox
  // Optional cache for the (expensive) max-path-lengths geometry scan: if the
  // file exists it is loaded, otherwise it is computed and saved there.
  std::string max_path_lengths_file;

  void validate() const {
    namespace fs = std::filesystem;
    if (tune.empty())
      throw std::runtime_error("genie_source: 'tune' must not be empty");
    if (seed <= 0)
      throw std::runtime_error(
          "genie_source: 'seed' must be > 0 (GENIE ignores non-positive "
          "seeds), got " +
          std::to_string(seed));
    auto require_file = [](std::string const& key, std::string const& path) {
      if (path.empty())
        throw std::runtime_error("genie_source: config key '" + key +
                                 "' is required");
      if (!fs::exists(path))
        throw std::runtime_error("genie_source: " + key + " '" + path +
                                 "' does not exist");
    };
    require_file("splines", spline_file);
    require_file("flux_file", flux_file);
    require_file("gdml_file", gdml_file);
    if (!max_path_lengths_file.empty() && !fs::exists(max_path_lengths_file)) {
      // Will be created after the geometry scan — its directory must exist.
      auto const dir = fs::path{max_path_lengths_file}.parent_path();
      if (!dir.empty() && !fs::is_directory(dir))
        throw std::runtime_error(
            "genie_source: cannot create max_path_lengths_file '" +
            max_path_lengths_file + "': directory '" + dir.string() +
            "' does not exist");
    }
  }
};

}  // namespace aegir
