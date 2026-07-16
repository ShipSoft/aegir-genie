<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Validation against gevgen_fnal

> **Note (geometry backend change).** This validation was performed with the
> previous geometry path (GDML imported into TGeo via GENIE's
> `ROOTGeomAnalyzer`, on the FairShip/TGeo-exported production GDML). The
> generator now analyzes the GeoModel-built Geant4 geometry directly
> (`ShipGeomAnalyzer`); the target-nucleus derivation (averaged-A ion per
> element) is unchanged. Max-path-lengths caches produced for the GDML
> geometry are invalid and must be regenerated, and the statistical battery
> below should be repeated on the GeoModel geometry once production inputs
> exist. See the next section for the analyzer-level validation of the new
> path.

## Geometry-analyzer validation (GeoModel path)

The new analyzer was validated against `ROOTGeomAnalyzer` on the *same*
detector description: the GeoModel geometry (`build_geometry` output)
exported to GDML with `G4GDMLParser` and fed to an old (pre-change) build,
both computing flux-scanned max path lengths from an identical synthetic
flux. Two findings:

1. **The GDML round trip is silently broken for the GeoModel geometry** —
   and is itself the strongest argument for the direct path. TGeo's GDML
   importer strips the exporter's `0x…` pointer suffixes; every element
   whose bare name then collides with a *material* name (Iron, Copper,
   Tungsten, Tantalum, Silicon, Lead — the pure metals are named after
   their element) fails to resolve and is dropped. Those materials import
   with **empty element lists** and Inconel718 loses its Fe fraction, so
   `ROOTGeomAnalyzer` saw 15 targets instead of 20: **no W/Ta (proton
   target), no Fe (muon shield), no Pb, no Cu** — and inflated alloy
   component weights from renormalisation over the surviving fractions.

2. **Where the old path is intact, the two analyzers agree exactly.** For
   every target living only in cleanly-imported materials (He-4, N-14,
   O-16, Al-27, Si-28, Ar-40, Ca-40) the independently computed max path
   lengths agree to all printed digits; every discrepant target traces to
   a material demonstrably mangled by the GDML import. The unit test
   battery (`tests/geom_analyzer_test.cpp`) additionally checks the
   machinery against analytic values on a known geometry.

End-to-end, the new path generates events on the production GeoModel
geometry with the production splines (tune `G18_02a_02_11b`; all 20
targets covered): a 2000-event `gevgen_ship` run completes with working
POT accounting and a physically sensible vertex-z distribution (muon
shield and downstream detectors populated, the low-density decay volume
nearly empty).

The embedded generation path was validated against GENIE's own
`gevgen_fnal` application — the tool the FairShip neutrino workflow
adopts — by generating samples with both on **identical inputs**: the
2026 min-bias gsimple flux, the full SHiP geometry (GDML export), the
production cross-section splines (tune `G18_02a_02_11b`, extended — see
below), and a shared max-path-lengths file, so both use the same global
probability scale.

The samples are statistically independent (different flux subsequences,
different RNG streams; `gevgen_fnal` randomises its start entry), so
the comparison is statistical: per observable a two-sample chi-squared
test and a Kolmogorov–Smirnov test (`scripts/compare_genie_samples.py`),
with Poisson bin errors — both samples are unweighted.

## Result

2000 events per tool. All observables statistically compatible:

| observable | chi2/ndf | p (chi2) | p (KS) |
|:--|--:|--:|--:|
| E_nu (all flavours) | 1.05 | 0.40 | 0.33 |
| E_nu (nu_mu) | 0.94 | 0.55 | 0.45 |
| vertex z | 0.98 | 0.50 | 0.98 |
| vertex material | 1.49 | 0.17 | — |
| final-state multiplicity | 1.49 | 0.061 | 0.46 |
| lepton momentum | 1.53 | 0.050 | 0.79 |
| Q^2 | 1.09 | 0.35 | 0.25 |
| W | 1.12 | 0.31 | 0.13 |

Normalisation: POT-equivalent for 2000 events was 4.14×10^7 (embedded,
own exposure accounting) vs 4.07×10^7 (`gevgen_fnal`, `AccumPOTs`) —
agreement within 1.8%, inside the ~2.2% statistical spread. The global
probability scales are identical by construction (shared `-m` file).

## Findings along the way

The cross-check surfaced three issues worth knowing about — none of
them a defect of either generator:

1. **Averaged-A target nuclides.** GENIE's `ROOTGeomAnalyzer` requests
   one nuclide per geometry element with the *natural-average* mass
   number rounded (isotope tables are ignored): natural Ni and Cu map
   to Ni-59 and Cu-64, which no isotope expansion contains.
   `gevgen_fnal` aborts (`Assertion fUseSplines' failed`) when they are
   missing from the spline file; the embedded driver silently fell back
   to slow numerical integration for the same channels — which had
   dominated the generation CPU time. Spline target lists must be
   derived with aegir's `gdml_target_nuclei.py` (union of isotope
   expansion and averaged-A nuclides).

2. **Block-ordered flux files bias small samples.** The merged 2026
   gsimple file stores the min-bias nu_mu bulk in its first ~18M
   entries and an importance-sampled flavour-balanced block after.
   `GSimpleNtpFlux` reads sequentially (from a random offset in
   `gevgen_fnal`), so any sample much smaller than one full cycle draws
   from a single block and inherits its composition — the first
   comparison attempt produced two mutually incompatible (and both
   unphysical) samples this way, with e.g. nu_e fractions of 2.8% vs
   35%. **Merged gsimple flux files must be shuffled** before use with
   `-n`-style event counts; after shuffling both tools agree (table
   above). Note that a shuffled-copy step (a ROOT macro) must read the
   input *sequentially* into memory — random-order `TTree` reads
   decompress one basket per entry and are ~1000× slower.

3. **Exposure accounting per window.** POT numbers reported by either
   tool refer to the flux-file window actually consumed. On the
   block-ordered file the two tools consumed windows with very
   different weight profiles, making their POT figures differ by the
   mean-weight ratio (~2.4×) — on the shuffled file they agree.

## Full-chain phlex run

The embedded plugin also ran the complete simulation chain under phlex
on the same inputs — `genie_source` → `geant4_module` (FTFP_BERT) →
RNTuple output — on the full SHiP geometry: 200 events in 80 s wall
(single-threaded Geant4, 95.7% CPU efficiency), peak RSS 10.3 GB
(GENIE ~3.4 GB + full-geometry Geant4 in one process). The 200 events
produced 2.63M simulated secondaries and 20 254 hits in the SBT and
timing-detector sensitive volumes.

Consistency: the plugin's 200 `mc_particles` collections are
**content-identical (200/200)** to the first 200 events of the
standalone `gevgen_ship` sample generated with the same configuration
and seed — the shared driver assembly and per-event reseeding give the
same event sequence through either entry point, on the production
geometry as on the test setup.

## Shuffling flux files

`scripts/shuffle_gsimple.C` produces a shuffled copy of a GSimple flux
file (seeded, reproducible; co-shuffles a filled `aux` branch; copies
`meta` unchanged; prints per-flavour totals and slice compositions as
built-in verification):

```sh
export ROOT_INCLUDE_PATH="$CONDA_PREFIX/include/GENIE"
root -l -b -q \
  -e 'gSystem->Load("libGFwEG"); gSystem->Load("libGTlFlx");' \
  'scripts/shuffle_gsimple.C+("in.root","out.root",12345)'
```

About four minutes and ~3 GB of memory for a 50M-entry file. Stage the
input locally first — and note the macro deliberately reads the input
sequentially into memory: shuffled-order tree reads decompress one
basket per entry and take hours instead.

## Reproducing

See the commands in this repository's README (generation) and
`scripts/compare_genie_samples.py --help` (comparison). The spline
extension for the averaged-A nuclides is part of the
`genie-splines-ship` package from build 101.
