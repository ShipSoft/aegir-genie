<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: LGPL-3.0-or-later
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
here**. aegir never links this code — phlex discovers the plugin at run time
via `PHLEX_PLUGIN_PATH`, and workflows refer to it only by its `cpp:` name.

The SHiP-authored source in this repository stays **LGPL-3.0-or-later** — the
same license as aegir — so it remains reusable and can move to a shared package
without GPL contamination. Every file here is therefore LGPL. What is GPL is the
*combined program*: the plugin binary linked against the GPL GENIE library must
be distributed under **GPL-3.0-or-later** (with which LGPL-3.0-or-later is
compatible), a term that comes from GENIE itself (whose own GPL text ships with
the GENIE package). This repository's sources carry no GPL-licensed file, so
`LICENSES/` holds only the LGPL text.

Two helper headers (`src/mc_particle_source.hpp`, `src/philox_rng.hpp`) are
vendored unchanged from aegir and keep their LGPL-3.0-or-later notices — now the
same license as the rest of this repository. They should move to a shared
package once one exists (tracked as follow-up in aegir's GENIE integration
plan).

Related, but without GENIE linkage (and therefore in aegir, not here):
the `genie_reader_source` plugin, which reads pre-generated GENIE rootracker
files with bare ROOT I/O and serves as the permanent validation path for this
plugin.

## Status

| Piece | State |
|---|---|
| `pixi install` | works |
| `genie_source` plugin | full chain (GENIE → Geant4 → output) validated on the production SHiP geometry |
| `gevgen_ship` app | validated against `gevgen_fnal` on identical inputs — see [docs/validation.md](docs/validation.md) |
| `ShipFluxDriver` (flux-file reading, unit conversions, exposure accounting) | works, unit-tested (`pixi run test`) |
| event generation (`GMCJDriver::GenerateEvent`) | works; embedded and `gevgen_ship` paths verified content-identical (incl. full geometry) |
| packaged cross-section splines | `genie-splines-ship` on prefix.dev/ship (production build; averaged-A extension from build 101) |

## Plugins and tools

| Name | Type | Description |
|--------|------|-------------|
| `genie_source` | phlex source | Embedded GENIE: flux × splines × GeoModel geometry → `MCParticle` vectors |
| `gevgen_ship` | CLI app | Same driver assembly, native GHEP output for validation (no phlex) |

Configuration keys (see `workflows/genie_st.jsonnet` for a full example):

| key | default | meaning |
|---|---|---|
| `tune` | `G18_02a_00_000` | GENIE comprehensive model tune; must match the splines |
| `splines` | *required* | cross-section spline XML (`gmkspl` output) |
| `flux_file` | *required* | neutrino flux file (format per `flux_format`; `root://` URLs work for gsimple) |
| `flux_format` | `ship` | `ship` (schema-v1 ntuple, see below) or `gsimple` (GENIE GSimple flux, the gevgen_fnal input format) |
| `geometry_file` | *required* | detector geometry: GeoModel SQLite db, the same file aegir's `geometry_geomodel_provider` loads (bare filenames resolve via `$SHIPGEOMETRY_ROOT/share/geometry`) |
| `top_volume` | *(unset)* | optional: restrict GENIE to this logical volume; unset scans the entire world |
| `seed` | `20260706` | base seed; each event reseeds GENIE via Philox for reproducibility |
| `max_path_lengths_file` | *(unset)* | optional XML cache for the max-path-lengths flux scan: loaded if it exists, otherwise computed and saved. The scan uses the flux, so the cache depends on **geometry and flux** — regenerate it when either changes. **A stale cache is loaded silently and skews the probability normalisation** — caches made for the old GDML/TGeo geometry are invalid |

The geometry is analyzed directly on the GeoModel-built Geant4 geometry
(`ShipGeomAnalyzer`, a `genie::GeomAnalyzerI` over the SHiP geometry
service's ray scanner) — no GDML/TGeo conversion sits between GENIE and the
geometry the Geant4 simulation tracks through. The target-nucleus list uses
one averaged-A ion per element (e.g. natural Cu → Cu-64), exactly as GENIE's
`ROOTGeomAnalyzer` derived it from the GDML, so the `genie-splines-ship`
spline package remains valid.

GENIE is not thread-safe (unsynchronised singletons, global `gRandom` /
`gGeoManager`), so the source runs serialised — same pattern as aegir's other
generator sources.

Two details make runs reproducible and byte-comparable across paths:

- **Every RNG stream GENIE draws from is reseeded per event**
  (`aegir::reseed_event`): GENIE's `RandomGen`, ROOT's global `gRandom`,
  *and* Pythia6's internal RANMAR generator (`MRPY` common block) — the last
  is a hidden stream that GENIE never reseeds and whose state would
  otherwise leak from one event's hadronization into the next.
- **Events are generated in event-number order.** Phlex's serialised
  scheduling guarantees mutual exclusion, not FIFO order (occasional swaps
  under TBB scheduling are observed in practice), and the flux driver is
  shared sequential state — so the source generates ahead in order and
  caches results when a later event is requested first. This assumes
  `generate_layers`' default 0-based contiguous event numbering.

Together, each event is a pure function of (config, seed, event number, flux
position), and a job is reproducible end to end. For debugging, set
`AEGIR_GENIE_RNG_TRACE=1` to print per-event seed/flux/vertex state to
stderr in both the plugin and `gevgen_ship`; the traces diff line by line.

## Flux input

Two flux formats are supported (config `flux_format` / CLI `--flux-format`):

**`ship`** — the SHiP neutrino flux ntuple (schema version 1, defined in
aegir's `docs/neutrino_flux.md`), read by `ShipFluxDriver` (a
`genie::GFluxI` / `GFluxExposureI` implementation): an RNTuple `nu_flux`
with one entry per neutrino ray — PDG code, production vertex (mm, ns),
momentum (GeV), statistical weight, parent information — plus a `flux_meta`
RNTuple with the file's POT equivalent and maximum energy. Every ray keeps
the exact energy–direction–position correlations from the upstream
production, and the POT bookkeeping replaces FairShip's fixed-σ yield
arithmetic (FairShip issue #984). `scripts/make_flux_ntuple.py` writes a
synthetic file for testing; real files are converted from FairShip
productions with aegir's `scripts/convert_fairship_nu_flux.py`.

**`gsimple`** — GENIE's GSimple flux format, read by GENIE's own
`genie::flux::GSimpleNtpFlux` driver. This is the format the SHiP neutrino
group produces as `gevgen_fnal` input, so the group's existing files (e.g.
on EOS, via `root://` URLs) work without conversion, and the embedded source
can be cross-checked against the `gevgen_fnal` CLI pipeline on identical
input. Facts worth knowing about the format and driver:

- Entry positions are **lab-frame meters** (time in seconds) — already the
  SI units `GFluxI::Position()` promises; we take the lab frame to be the
  SHiP global frame, i.e. the frame of the GeoModel geometry. At startup the
  driver assembly logs the first flux ray next to the geometry bounding box
  (`[genie-driver] frame check: ...`), so a frame or unit mismatch is
  visible immediately.
- **Weighted rays are unweighted internally**: by default (`SetGenWeighted`
  never called — same as `gevgen_fnal`) the driver accept–rejects entries on
  `wgt/maxWgt`, drawing from GENIE's `RandomGen` (covered by the per-event
  reseeding), and reports `Weight() = 1`. Combined with
  `ForceSingleProbScale`, generated events are unweighted. POT accounting
  comes from the driver's `GFluxExposureI` implementation (effective POT per
  used entry = `protons/nEntries/maxWgt`).
- The driver is configured with `"no-offset-index"` (start at entry 0
  instead of a `RandomGen`-chosen random offset — `gevgen_fnal` uses the
  random offset) and `SetNumOfCycles(0)` (recycle the file indefinitely,
  like `gevgen_fnal`).
- The current SHiP production file
  (`Mbias2026noCharm_makeCascade_merged_nuflux_gsimple.root`, 49.9M rays,
  1.7568e9 POT, all six flavours, max 270.2 GeV, weights 0.081–1.0) carries
  an `aux` branch that is **empty** (no aux names declared in the meta, no
  values) — parent/ancestry information is not available from it, unlike
  schema-v1 ship files.
- **Stage the file locally and shuffle it first.** Entry-wise xrootd reads
  stall the max-path-lengths flux scan, and merged files are typically
  flavour-block-ordered, which biases any sample smaller than one full file
  cycle — shuffle once with `scripts/shuffle_gsimple.C` (seeded,
  reproducible, verifies itself; see
  [docs/validation.md](docs/validation.md)).
- Reproducibility caveat: after a max-path-lengths flux scan the GSimple
  driver's entry cursor cannot be rewound (unlike `ShipFluxDriver`), so a
  run that computes the scan and a run that loads it from the cache consume
  different flux entries. Runs are byte-reproducible when the cache state is
  the same (both fresh or both cached); the physics is unaffected. With
  `flux_format: ship`, runs are byte-identical regardless of the cache
  state.

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

## Running

```sh
pixi run phlex -c workflows/genie_st.jsonnet
```

`activate.sh` puts this repo's `build/` directory on `PHLEX_PLUGIN_PATH` and
exports `GENIE` (the runtime configuration area). The geometry, Geant4 and
output plugins in the example workflow come from aegir: point
`PHLEX_PLUGIN_PATH` at an aegir build/install as well, or install the aegir
conda package into this environment.

Note that while this repo's environment includes the `geant4` package (the
geometry analyzer navigates the converted Geant4 geometry), it does not set
up the Geant4 *data* files: full workflows with `geant4_module` fail here
with unset Geant4 data variables (`G4ENSDFSTATEDATA` etc.). Full chains
would run from an aegir environment with this repo's `build/` appended to
`PHLEX_PLUGIN_PATH` (and `GENIE` pointing at this environment's
`share/genie`). Generator-only workflows (genie source +
`sim_output_module` in `mc_only` mode) run here directly.

**Known limitation:** the in-process combination of `genie_source` and
aegir's `geant4_module` currently segfaults during aegir's geometry
construction — Geant4 MT permits only one geometry-creating thread per
process, and the two plugins each convert their own geometry on different
threads (issue #11; upstream Geant4 bug #2747). Until the plugins share
one geometry, run GENIE events through Geant4 via the two-step path:
`gevgen_ship` → `gntpc -f rootracker` → aegir's `genie_reader_source`.

## Validating the embedded source: `gevgen_ship`

The plugin's physics must be checkable independently of phlex. `gevgen_ship`
is a plain command-line generator that assembles the **identical** machinery
(`src/genie_driver_setup.hpp`: same `ShipFluxDriver`, `ShipGeomAnalyzer`,
`GMCJDriver` settings, same per-event reseeding and generation order —
identical config and seed give the **exactly identical** event sequence in
both paths, verified event-for-event on a 200-event smoke sample), but
writes native GENIE GHEP output via `genie::NtpWriter`:

```sh
pixi run ./build/gevgen_ship -f nu_flux.root -g ship_geometry.db \
    -x gxspl-ship.xml \
    -n 1000 -o genie_events            # -> genie_events.0.ghep.root
pixi run gntpc -i genie_events.0.ghep.root -f rootracker
```

(`pixi run install` puts `gevgen_ship` on the environment's PATH.)

The rootracker file reads back through aegir's `genie_reader_source`, so the
two paths compare apples to apples:

- **embedded**: `genie_source` → `MCParticle` → Geant4 (this repo), and
- **file-based**: `gevgen_ship` → `gntpc -f rootracker` →
  `genie_reader_source` → `MCParticle` → Geant4 (aegir only),

with the same flux, geometry, tune and splines — differences isolate the
in-situ integration rather than the physics. `gevgen_ship` prints its
normalisation at the end (flux rays used, `GlobProbScale`, POT equivalent =
fraction of the flux file consumed × its POT), so generated samples carry
their exposure. `gevgen_ship --help` lists all options; `--dry-run` stops
after `GMCJDriver::Configure()` to check tune/splines/geometry/flux wiring
without generating.

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
4. Validate against the `genie_reader_source` path (same flux + geometry
   through `gevgen_ship` + `gntpc -f rootracker`, see above): vertex
   material/z distributions, energy spectra, final-state multiplicities
   (aegir plan, Phase 4).

Follow-ups beyond first events: POT/exposure data product (needs a data-model
addition), unweighted flux generation (accept–reject on ray weights),
per-event interaction summaries.
