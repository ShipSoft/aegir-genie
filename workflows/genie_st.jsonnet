// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// genie_st.jsonnet — GENIE neutrino interactions, single-threaded chain.
//
// Illustrative workflow: the genie source block below is what this repo
// provides; the geometry / geant4 / output blocks are aegir plugins (the
// config blocks are inlined here so this file stands alone — with aegir's
// workflow library on hand, compose lib.driver / lib.geant4 / lib.full_output
// with the genie block instead). phlex discovers all plugins via
// PHLEX_PLUGIN_PATH, so aegir's build/install directory must be on it
// alongside this repo's build directory (activate.sh takes care of the
// latter).
//
// Runtime inputs that must exist (see README.md § Remaining work):
//   - cross-section splines for the SHiP target nuclei (gmkspl output),
//   - a schema-v1 neutrino flux file (scripts/make_flux_ntuple.py writes a
//     synthetic one; scripts/convert_fairship_nu_flux.py in aegir converts
//     real productions),
//   - the GeoModel geometry db (the same file the geant4 geomodel provider
//     loads; bare filenames resolve via $SHIPGEOMETRY_ROOT/share/geometry).
{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 10 },
    },
  },
  sources: {
    genie: {
      cpp: 'genie_source',
      // GENIE comprehensive model tune; must match the splines exactly.
      tune: 'G18_02a_02_11b',
      splines: 'gxspl-ship.xml',
      flux_file: 'nu_flux.root',
      // 'ship' (schema-v1 ntuple) or 'gsimple' (GENIE GSimple flux, the
      // gevgen_fnal input format the neutrino group produces; root:// URLs
      // work). See README.md § Flux input.
      flux_format: 'ship',
      geometry_file: 'ship_geometry.db',
      seed: 20260706,
      // Cache for the max-path-lengths flux scan: computed and saved on the
      // first run, loaded on subsequent runs. Depends on geometry AND flux —
      // regenerate when either changes.
      max_path_lengths_file: 'maxpl-ship.xml',
    },
    // aegir plugins from here on.
    field: { cpp: 'field_null_provider' },
    geometry: {
      cpp: 'geometry_geomodel_provider',
      db_file: 'ship_geometry.db',  // the same db genie analyzes
      sensitive_volumes: [],
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      verbosity: 0,
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'genie_st_output.root',
      histo_file: 'genie_st_validation.root',
    },
  },
}
