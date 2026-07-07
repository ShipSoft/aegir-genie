// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: GPL-3.0-or-later

// ship_flux_driver.hpp — GENIE flux driver reading SHiP neutrino flux files
//
// Implements genie::GFluxI (and the GFluxExposureI extension for POT
// accounting) on top of the SHiP neutrino-flux ntuple, schema version 1
// (see docs/neutrino_flux.md in aegir): an RNTuple `nu_flux` with one entry
// per neutrino ray (production vertex in mm/ns, momentum in GeV, statistical
// weight) plus an `flux_meta` RNTuple carrying the POT-equivalent of the
// file and the maximum neutrino energy.
//
// Each entry defines one flux ray exactly — energy, direction and production
// point stay correlated, unlike the histogram-based FairShip flux files.
// Positions are converted mm -> m and ns -> s for GENIE (SI units), the
// 4-momentum is completed with E = |p| (massless neutrinos).

#pragma once

#include <TLorentzVector.h>

#include <cstdint>
#include <memory>
#include <string>

#include "Framework/EventGen/GFluxI.h"
#include "Framework/ParticleData/PDGCodeList.h"
#include "Tools/Flux/GFluxExposureI.h"

namespace aegir {

class ShipFluxDriver : public genie::GFluxI,
                       public genie::flux::GFluxExposureI {
 public:
  // Opens the flux file and reads its metadata; throws std::runtime_error
  // with a descriptive message if the file is missing, has no nu_flux /
  // flux_meta ntuples, or declares an unsupported schema version.
  // With cycle = true the driver restarts from the first entry when the file
  // is exhausted (exposure keeps accumulating); with cycle = false End()
  // becomes true instead.
  explicit ShipFluxDriver(std::string const& path, bool cycle = false);
  ~ShipFluxDriver() override;

  // GFluxI interface
  genie::PDGCodeList const& FluxParticles() override;
  double MaxEnergy() override { return max_energy_; }
  bool GenerateNext() override;
  int PdgCode() override { return pdg_; }
  double Weight() override { return weight_; }
  TLorentzVector const& Momentum() override { return p4_; }
  TLorentzVector const& Position() override { return x4_; }
  bool End() override { return end_; }
  long int Index() override { return index_; }
  void Clear(Option_t* opt) override;
  void GenerateWeighted(bool gen_weighted) override;

  // GFluxExposureI interface (kPOTs): the file represents pot() protons on
  // target; the exposure delivered so far scales with the fraction of
  // entries used (cycling accumulates across passes).
  double GetTotalExposure() const override;
  long int NFluxNeutrinos() const override { return n_used_; }

  // SHiP-specific accessors
  double pot() const { return pot_; }
  std::uint64_t entries() const { return n_entries_; }

  // First ray of the file (SI units, like Position()/Momentum()), read
  // without disturbing the sequential driver state — for startup
  // frame-sanity logging.
  struct Ray {
    int pdg;
    TLorentzVector position;  // m, s
    TLorentzVector momentum;  // GeV
  };
  Ray peek_first_ray();

 private:
  struct Reader;  // hides ROOT RNTuple types from this header
  std::unique_ptr<Reader> reader_;

  std::string path_;
  bool cycle_ = false;
  bool gen_weighted_ = true;  // rays carry their weight either way

  // file metadata (flux_meta)
  double pot_ = 0.0;
  double max_energy_ = 0.0;
  std::uint64_t n_entries_ = 0;

  // current ray
  long int index_ = -1;  // entry number of the current ray
  int pdg_ = 0;
  double weight_ = 0.0;
  TLorentzVector p4_;  // GeV
  TLorentzVector x4_;  // SI: m, s
  bool end_ = false;
  long int n_used_ = 0;  // rays generated so far (across cycles)

  genie::PDGCodeList pdg_list_;
  bool pdg_list_built_ = false;
};

}  // namespace aegir
