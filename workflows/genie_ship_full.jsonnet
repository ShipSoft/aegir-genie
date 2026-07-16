// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// Full end-to-end chain on the production SHiP geometry: embedded GENIE
// (gsimple flux, production splines) -> Geant4 -> RNTuple output. This is
// the configuration validated in docs/validation.md.
//
// KNOWN LIMITATION (issue #11): this in-process combination of genie_source
// and geant4_module currently segfaults during aegir's geometry construction
// (Geant4 MT allows only one geometry-creating thread per process; upstream
// Geant4 bug #2747). Use the two-step path meanwhile: gevgen_ship ->
// gntpc -f rootracker -> aegir's genie_reader_source.
//
// Render with the input locations as external variables, e.g.
//   jsonnet -V inputs=/path/to/inputs -V geometry=ship_geometry.db \
//       workflows/genie_ship_full.jsonnet
// where `inputs` holds the (locally staged! never xrootd) shuffled gsimple
// flux, the spline XML, and the max-path-lengths cache, and `geometry` is a
// GeoModel db path (bare filenames resolve via
// $SHIPGEOMETRY_ROOT/share/geometry). Run from an aegir environment (Geant4
// data) with this repo's build/ on PHLEX_PLUGIN_PATH and GENIE pointing at
// this repo's env share/genie — see README.md.
local S = std.extVar('inputs');
local geometry = std.extVar('geometry');
{
  driver: { cpp: 'generate_layers', layers: { event: { total: 200 } } },
  sources: {
    genie: {
      cpp: 'genie_source',
      tune: 'G18_02a_02_11b',
      splines: S + '/gxspl-ship.xml',
      flux_file: S + '/gsimple_flux_shuffled.root',
      flux_format: 'gsimple',
      // The GeoModel world *is* the cave (no surrounding GDML super-world),
      // so no top_volume restriction is needed.
      geometry_file: geometry,
      seed: 20260709,
      // Computed and saved on the first run; flux-dependent (see README).
      max_path_lengths_file: S + '/maxpl_ship.xml',
    },
    field: { cpp: 'field_null_provider' },
    geometry: {
      cpp: 'geometry_geomodel_provider',
      db_file: geometry,  // the same db genie analyzes
      sensitive_volumes: ['sbt_sensors', 'TimDetBar'],
    },
  },
  modules: {
    geant4: { cpp: 'geant4_module', physics_list: 'FTFP_BERT', concurrency: 1, verbosity: 0 },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'genie_ship_full_output.root',
      histo_file: 'genie_ship_full_validation.root',
    },
  },
}
