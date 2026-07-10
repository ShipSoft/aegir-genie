<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Validation against gevgen_fnal

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

## Reproducing

See the commands in this repository's README (generation) and
`scripts/compare_genie_samples.py --help` (comparison). The spline
extension for the averaged-A nuclides is part of the
`genie-splines-ship` package from build 101.
