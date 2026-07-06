# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Write a synthetic SHiP neutrino flux file (schema v1).

Produces the two RNTuples the GENIE flux driver reads — ``nu_flux`` (one
entry per neutrino ray) and ``flux_meta`` (POT equivalent, maximum energy) —
filled with a plausible toy spectrum. Useful for exercising the flux driver
and the genie source without a real production; for real flux files convert
FairShip productions with aegir's ``scripts/convert_fairship_nu_flux.py``.

Usage: python make_flux_ntuple.py [output.root] [n_entries]
"""

import os
import random
import sys

import ROOT

out = sys.argv[1] if len(sys.argv) > 1 else "nu_flux.root"
n_entries = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
random.seed(7)

model = ROOT.RNTupleModel.Create()
model.MakeField["std::int32_t"]("pdg")
for name in ("vx", "vy", "vz", "t", "px", "py", "pz", "weight",
             "parent_px", "parent_py", "parent_pz"):
    model.MakeField["double"](name)
model.MakeField["std::int32_t"]("parent_pdg")
model.MakeField["std::int32_t"]("process_id")
model.MakeField["std::int64_t"]("origin_run")
model.MakeField["std::int64_t"]("origin_event")

writer = ROOT.RNTupleWriter.Recreate(model, "nu_flux", out)
entry = writer.CreateEntry()
emax = 0.0
for i in range(n_entries):
    pdg = random.choice([12, -12, 14, -14, 16, -16])
    e = random.expovariate(1 / 15.0) + 0.5
    emax = max(emax, e)
    entry["pdg"] = pdg
    entry["vx"] = random.gauss(0, 100.0)  # mm
    entry["vy"] = random.gauss(0, 100.0)
    entry["vz"] = random.uniform(-50000.0, -40000.0)
    entry["t"] = random.uniform(0, 5)  # ns
    entry["px"] = e * random.gauss(0, 0.02)  # GeV
    entry["py"] = e * random.gauss(0, 0.02)
    entry["pz"] = e
    entry["weight"] = 1.0 if pdg % 14 else 326.0 / 768.75
    entry["parent_pdg"] = random.choice([211, 321, 13, 431])
    entry["parent_px"] = 0.0
    entry["parent_py"] = 0.0
    entry["parent_pz"] = e * 2
    entry["process_id"] = 0
    entry["origin_run"] = 1000 + i // 100
    entry["origin_event"] = i
    writer.Fill(entry)
del writer

meta = ROOT.RNTupleModel.Create()
meta.MakeField["std::int32_t"]("schema_version")
meta.MakeField["double"]("pot")
meta.MakeField["double"]("max_energy")
meta.MakeField["std::string"]("description")
meta.MakeField["std::string"]("software")
f = ROOT.TFile.Open(out, "UPDATE")
mwriter = ROOT.RNTupleWriter.Append(meta, "flux_meta", f)
mentry = mwriter.CreateEntry()
mentry["schema_version"] = 1
mentry["pot"] = 6.5041e10
mentry["max_energy"] = emax
mentry["description"] = "synthetic toy flux (schema v1)"
mentry["software"] = "aegir-genie make_flux_ntuple.py"
mwriter.Fill(mentry)
del mwriter
f.Close()

print(f"wrote {out}: {n_entries} rays, max energy {emax:.1f} GeV")
sys.stdout.flush()
# PyROOT teardown can hang after RNTuple writes; exit hard once done.
os._exit(0)
