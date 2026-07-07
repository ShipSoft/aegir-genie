// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// philox_rng.hpp — counter-based RNG shared by the event-generator sources
//
// Vendored from aegir (src/philox_rng.hpp) pending a shared helper package;
// keep in sync with the original (the backward-compatible `ctr1` sub-stream
// parameter below is pending upstreaming). LGPL-3.0-or-later, see README.md
// § Licensing.
//
// Random123 Philox 4x32 is deterministic per seed with no shared state, so
// each event seeds a fresh instance and generation is reproducible and
// thread-safe by construction.
//
// The counter advances sequentially (ctr[0]++ per 4-word block) and the
// Philox output block is buffered; uniform() returns successive words of the
// buffered output. This preserves Philox's guaranteed period — an earlier
// version fed the output back into the counter (output-feedback mode) and
// returned ctr[0] + 1 as its first word, forfeiting both the period and the
// intended first draw.

#pragma once

#include <Random123/philox.h>

#include <cstdint>

namespace aegir {

class PhiloxRng {
 public:
  // key_hi selects an independent stream, so different generators seeded with
  // the same event number draw uncorrelated sequences. ctr1 initializes the
  // second counter word, giving each (seed, key_hi, ctr1) triple a disjoint
  // counter range — use it for per-event sub-streams of one seed without
  // perturbing the key (a key derived as seed ^ event would collide across
  // seeds: XOR is not injective in (seed, event)).
  explicit PhiloxRng(std::uint32_t seed, std::uint32_t key_hi = 0xBEEFCAFE,
                     std::uint32_t ctr1 = 0)
      : key_{{seed, key_hi}}, ctr_{{0, ctr1, 0, 0}} {}

  double uniform() {
    if (idx_ >= 4) {
      buf_ = rng_(ctr_, key_);
      ctr_[0]++;
      idx_ = 0;
    }
    // Map a 32-bit word to [0, 1)
    return buf_[idx_++] * (1.0 / 4294967296.0);
  }

  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

 private:
  r123::Philox4x32 rng_;
  r123::Philox4x32::key_type key_;
  r123::Philox4x32::ctr_type ctr_;
  r123::Philox4x32::ctr_type buf_{};
  int idx_ = 4;
};

}  // namespace aegir
