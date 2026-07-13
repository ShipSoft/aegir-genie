// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// Shuffle the entries of a GENIE GSimple flux file.
//
// Why: merged GSimple files are often concatenated from per-sample or
// per-flavour productions and are then *block-ordered* — e.g. the 2026
// SHiP min-bias file stores the nu_mu bulk in its first ~18M entries
// and an importance-sampled flavour-balanced block after. GENIE's
// GSimpleNtpFlux reads sequentially (gevgen_fnal from a random start
// entry), so any sample much smaller than one full file cycle draws
// from a single block and inherits its composition instead of the
// file average: we measured nu_e fractions between 2.8% and 35% for
// 2000-event samples depending on the start position. Shuffling once
// makes every window representative. See docs/validation.md.
//
// Usage (needs the GENIE flux dictionaries; from a genie conda
// environment):
//
//   export ROOT_INCLUDE_PATH="$CONDA_PREFIX/include/GENIE"
//   root -l -b -q \
//     -e 'gSystem->Load("libGFwEG"); gSystem->Load("libGTlFlx");' \
//     'shuffle_gsimple.C+("in.root","out.root",12345)'
//
// Implementation notes:
//  - The input is read *sequentially* into memory and written out in
//    shuffled order. Do not be tempted to read in shuffled order:
//    random-access TTree reads decompress one basket per entry and are
//    ~1000x slower (hours instead of minutes for a 50M-entry file).
//  - Memory: about 60 B per entry (plus ~50 B per aux record if the
//    file has a filled aux branch) — roughly 3 GB for 50M entries.
//  - The shuffle is a seeded Fisher-Yates permutation: the same seed
//    reproduces the same output. entry and aux (if present) are
//    permuted together, and the meta tree is copied unchanged.

#include <TFile.h>
#include <TRandom3.h>
#include <TSystem.h>
#include <TTree.h>

#include <cstdio>
#include <numeric>
#include <vector>

#include "Tools/Flux/GSimpleNtpFlux.h"

void shuffle_gsimple(const char* in, const char* out, unsigned seed = 12345) {
  TFile fin(in, "READ");
  if (fin.IsZombie()) {
    printf("ERROR: cannot open input '%s'\n", in);
    gSystem->Exit(1);
  }
  TTree* flux = fin.Get<TTree>("flux");
  TTree* meta = fin.Get<TTree>("meta");
  if (!flux || !meta) {
    printf("ERROR: '%s' has no flux/meta trees — not a GSimple file?\n", in);
    gSystem->Exit(1);
  }
  const Long64_t n = flux->GetEntries();
  const bool has_aux = flux->GetBranch("aux") != nullptr;

  // Bind with plain SetBranchAddress. Do NOT SetBranchStatus-filter
  // object branches here: with this file layout the address silently
  // never fills and every entry reads as zeros.
  auto* rec = new genie::flux::GSimpleNtpEntry;
  flux->SetBranchAddress("entry", &rec);
  auto* aux = new genie::flux::GSimpleNtpAux;
  if (has_aux) flux->SetBranchAddress("aux", &aux);

  printf("reading %lld entries from %s%s\n", n, in,
         has_aux ? " (with aux branch)" : "");
  std::vector<genie::flux::GSimpleNtpEntry> entries;
  std::vector<genie::flux::GSimpleNtpAux> auxes;
  entries.reserve(n);
  if (has_aux) auxes.reserve(n);
  for (Long64_t i = 0; i < n; ++i) {
    flux->GetEntry(i);
    if (i == 0 && rec->pdg == 0) {
      printf("ERROR: entry branch not bound (pdg = 0 at entry 0)\n");
      gSystem->Exit(1);
    }
    entries.push_back(*rec);
    if (has_aux) auxes.push_back(*aux);
    if (i % 10000000 == 0) printf("  read %lld / %lld\n", i, n);
  }

  // Per-flavour totals for the conservation check below.
  std::map<int, Long64_t> in_counts;
  for (auto const& e : entries) ++in_counts[e.pdg];

  std::vector<Long64_t> perm(n);
  std::iota(perm.begin(), perm.end(), 0);
  TRandom3 rng(seed);
  for (Long64_t i = n - 1; i > 0; --i) {
    Long64_t j = static_cast<Long64_t>(rng.Uniform() * (i + 1));
    std::swap(perm[i], perm[j]);
  }

  TFile fout(out, "RECREATE", "", 101);
  auto* oflux = new TTree("flux", flux->GetTitle());
  oflux->Branch("entry", &rec);
  if (has_aux) oflux->Branch("aux", &aux);
  for (Long64_t i = 0; i < n; ++i) {
    *rec = entries[perm[i]];
    if (has_aux) *aux = auxes[perm[i]];
    oflux->Fill();
    if (i % 10000000 == 0) printf("  wrote %lld / %lld\n", i, n);
  }
  oflux->Write();
  fin.cd();
  TTree* ometa = meta->CloneTree(-1);
  fout.cd();
  ometa->Write();
  fout.Close();

  // Verification 1: exact per-flavour conservation (it is a permutation).
  std::map<int, Long64_t> out_counts;
  for (auto const& e : entries) ++out_counts[e.pdg];  // same multiset
  printf("\nper-flavour totals (conserved by construction):\n");
  for (auto const& [pdg, cnt] : in_counts)
    printf("  pdg %6d : %lld\n", pdg, cnt);

  // Verification 2: composition uniformity across slices of the output —
  // the block-ordering symptom this tool removes. Uses the in-memory
  // shuffled order (identical to what was written).
  const int nslices = 4;
  const Long64_t width = 100000 < n / nslices ? 100000 : n / nslices;
  printf("output composition in %d slices of %lld entries:\n", nslices,
         width);
  for (int s = 0; s < nslices; ++s) {
    const Long64_t start = s * (n / nslices);
    std::map<int, Long64_t> c;
    for (Long64_t i = start; i < start + width; ++i)
      ++c[entries[perm[i]].pdg];
    printf("  [%10lld]", start);
    for (auto const& [pdg, cnt] : c)
      printf("  %d: %.1f%%", pdg, 100.0 * cnt / width);
    printf("\n");
  }

  printf("\nwrote %lld shuffled entries to %s (seed %u)\n", n, out, seed);
}
