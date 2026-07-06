<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: GPL-3.0-or-later
-->

# aegir-genie

GENIE neutrino event generation for the SHiP experiment's
[aegir](https://github.com/ShipSoft/aegir) simulation framework, embedded as
a phlex source plugin.

Instead of the multi-step command-line pipeline used by FairShip
(`gmkspl` → `gevgen` → `gntpc` → vertex re-placement), this plugin drives the
GENIE library directly, the way DUNE/NOvA/MicroBooNE (nutools `GENIEHelper`)
and KM3NeT (gSeaGen) do: a `genie::GMCJDriver` convolves the SHiP neutrino
flux with cross-section splines and the detector geometry, placing
interaction vertices consistently with the material the simulation tracks
through, with built-in protons-on-target accounting.

## Licensing — why this is a separate repository

GENIE is distributed under the GNU GPL, so anything that links it becomes a
combined work under GPL terms. aegir itself is LGPL-3.0-or-later and must not
acquire a GENIE dependency; therefore **everything that links GENIE lives
here**, licensed **GPL-3.0-or-later**. aegir never links this code — phlex
discovers the plugin at run time via `PHLEX_PLUGIN_PATH`, and workflows refer
to it only by its `cpp:` name.

Two helper headers (`src/mc_particle_source.hpp`, `src/philox_rng.hpp`) are
vendored unchanged from aegir and keep their original LGPL-3.0-or-later
notices; incorporating LGPL code in a GPL project is permitted. They should
move to a shared package once one exists (tracked as follow-up in aegir's
GENIE integration plan).

Related, but without GENIE linkage (and therefore in aegir, not here):
the `genie_reader_source` plugin, which reads pre-generated GENIE rootracker
files with bare ROOT I/O and serves as the permanent validation path for this
plugin.

## Status

| Piece | State |
|---|---|
| `pixi install` | works |
| `genie_source` plugin | compiles and links against the GENIE 3.06.02 conda package |
| `ShipFluxDriver` (flux-file reading, unit conversions, exposure accounting) | works, unit-tested (`pixi run test`) |
| constructor-time config validation | works, unit-tested |
| event generation (`GMCJDriver::GenerateEvent`) | **blocked — no cross-section splines packaged yet** (see below) |

## Plugins

| Plugin | Type | Description |
|--------|------|-------------|
| `genie_source` | Source | Embedded GENIE: flux × splines × TGeo geometry → `MCParticle` vectors |

Configuration keys (see `workflows/genie_st.jsonnet` for a full example):

| key | default | meaning |
|---|---|---|
| `tune` | `G18_02a_00_000` | GENIE comprehensive model tune; must match the splines |
| `splines` | *required* | cross-section spline XML (`gmkspl` output) |
| `flux_file` | *required* | SHiP neutrino flux ntuple, schema v1 (see below) |
| `gdml_file` | *required* | detector geometry, imported with `TGeoManager::Import` |
| `top_volume` | `World` | TGeo top volume for the geometry analyzer |
| `seed` | `20260706` | base seed; each event reseeds GENIE via Philox for reproducibility |
| `max_path_lengths_file` | *(unset)* | optional XML cache for the expensive geometry scan: loaded if it exists, otherwise computed and saved |

GENIE is not thread-safe (unsynchronised singletons, global `gRandom` /
`gGeoManager`), so the source runs serialised — same pattern as aegir's other
generator sources.

## Flux input

The flux enters through `ShipFluxDriver`, a `genie::GFluxI` /
`GFluxExposureI` implementation reading the SHiP neutrino flux ntuple
(schema version 1, defined in aegir's `docs/neutrino_flux.md`): an RNTuple
`nu_flux` with one entry per neutrino ray — PDG code, production vertex
(mm, ns), momentum (GeV), statistical weight, parent information — plus a
`flux_meta` RNTuple with the file's POT equivalent and maximum energy.
Every ray keeps the exact energy–direction–position correlations from the
upstream production, and the POT bookkeeping replaces FairShip's fixed-σ
yield arithmetic (FairShip issue #984).

`scripts/make_flux_ntuple.py` writes a synthetic file for testing; real
files are converted from FairShip productions with aegir's
`scripts/convert_fairship_nu_flux.py`.

## Building

```sh
pixi run build   # configure + build (plugin and tests)
pixi run test    # standalone flux-driver / config-validation checks
```

The `genie` conda package (pythia6-hadronization variant, built against the
C++23 ROOT like phlex) lives in the same pixi environment as everything
else. GENIE's shared libraries record no dependencies of their own, so all
of its transitive libraries — log4cpp, libxml2, LHAPDF, GSL, Pythia6,
ROOT's EGPythia6 — are linked explicitly (see `CMakeLists.txt`).

Until the genie package is published to `prefix.dev/ship`, `pixi.toml`
points at a local channel
(`file:///home/olantwin/Projects/SHiP/ship-conda-recipes-genie/output`);
drop that entry once it is published.

## Running

```sh
pixi run phlex -c workflows/genie_st.jsonnet
```

`activate.sh` puts this repo's `build/` directory on `PHLEX_PLUGIN_PATH` and
exports `GENIE` (the runtime configuration area). The geometry, Geant4 and
output plugins in the example workflow come from aegir: point
`PHLEX_PLUGIN_PATH` at an aegir build/install as well, or install the aegir
conda package into this environment.

## Remaining work for first generated events

1. **Cross-section splines** — the hard blocker. Package `gmkspl` output for
   the SHiP target nuclei (W, Fe, Pb, Si, …) over the SHiP energy range for
   the chosen tune(s) as a `genie-splines-ship` conda package (aegir GENIE
   plan, Phase 2), or document a one-off `gmkspl` production. The spline file
   must match the configured tune exactly.
2. **LHAPDF data** — the tune's PDF set (e.g. GRV98lo for G18 tunes) must be
   fetchable/installed where LHAPDF finds it (`lhapdf install GRV98lo` or a
   data package); only the LHAPDF library ships with the conda package.
3. **A real flux file** — convert a FairShip neutrino production to schema v1
   (weights and POT included).
4. **The detector GDML** — export the production geometry (for GeoModel-built
   geometry via `G4GDMLParser`, aegir plan Phase 3) so GENIE's TGeo import
   and Geant4 track the identical geometry.
5. Validate against the `genie_reader_source` path (same flux + geometry
   through `gevgen_fnal`): vertex material/z distributions, energy spectra,
   final-state multiplicities (aegir plan, Phase 4).

Follow-ups beyond first events: POT/exposure data product (needs a data-model
addition), unweighted flux generation (accept–reject on ray weights),
per-event interaction summaries.
