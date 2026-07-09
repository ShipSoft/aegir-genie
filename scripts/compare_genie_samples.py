#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Statistically compare two GENIE rootracker samples.

Compares event samples produced by different generation paths (e.g. the
embedded `gevgen_ship` and GENIE's `gevgen_fnal`) on identical inputs.
The samples are statistically independent — different flux subsequences
and RNG streams — so the comparison uses two-sample tests with proper
uncertainties, not equality: per observable a chi-squared test
(`TH1::Chi2Test`, "UU NORM": two unweighted samples, normalisation-free)
and a Kolmogorov-Smirnov test, flagging p-values below the threshold.

Both samples must be unweighted (single probability scale), so bin
errors are Poisson and the ROOT tests apply directly.

Observables: neutrino energy (per dominant flavour and combined),
interaction-vertex z, vertex material (via TGeo lookup in the supplied
geometry), visible final-state multiplicity, final-state lepton
momentum, and Q^2 and W reconstructed from the neutrino and lepton
StdHep entries (struck nucleon approximated at rest — identical
treatment for both samples).

Usage:
  compare_genie_samples.py A.rootracker.root B.rootracker.root \
      --gdml geometry.gdml [--labels ship fnal] [-o hists.root] \
      [--p-threshold 0.01]

Exit status: 0 when no test falls below the threshold, 1 otherwise.
"""

import argparse
import math
import sys

import ROOT

NEUTRINOS = (12, -12, 14, -14, 16, -16)
LEPTON_OF = {12: 11, 14: 13, 16: 15}
M_NUCLEON = 0.9389  # GeV, isoscalar average


def booking(tag):
    h = {
        "enu": ROOT.TH1D(f"h_enu_{tag}", "E_{#nu};E_{#nu} [GeV];events", 25, 0, 100),
        "enu_numu": ROOT.TH1D(
            f"h_enu_numu_{tag}", "E_{#nu} (#nu_{#mu});E_{#nu} [GeV];events", 25, 0, 100
        ),
        "vz": ROOT.TH1D(f"h_vz_{tag}", "vertex z;z [m];events", 30, 0, 150),
        "mult": ROOT.TH1D(
            f"h_mult_{tag}", "final-state multiplicity;N_{FS};events", 30, 0, 60
        ),
        "plep": ROOT.TH1D(
            f"h_plep_{tag}", "lepton momentum;p_{lep} [GeV];events", 25, 0, 100
        ),
        "q2": ROOT.TH1D(f"h_q2_{tag}", "Q^{2};Q^{2} [GeV^{2}];events", 25, 0, 25),
        "w": ROOT.TH1D(f"h_w_{tag}", "W;W [GeV];events", 25, 0, 10),
        "mat": ROOT.TH1D(f"h_mat_{tag}", "vertex material;;events", 1, 0, 1),
    }
    h["mat"].SetCanExtend(ROOT.TH1.kAllAxes)
    for hist in h.values():
        hist.SetDirectory(ROOT.nullptr)
        hist.Sumw2()
    return h


def fill_from_rootracker(path, hists, geo):
    f = ROOT.TFile.Open(path)
    if not f or f.IsZombie():
        raise SystemExit(f"error: cannot open {path}")
    t = f.Get("gRooTracker")
    if not t:
        raise SystemExit(f"error: no gRooTracker tree in {path}")

    # Leaf-based access: PyROOT's branch-attribute pythonization cannot
    # handle the 2D StdHepP4[StdHepN][4] leaf (stoi abort).
    leaf = {
        name: t.GetLeaf(name)
        for name in ("StdHepN", "StdHepPdg", "StdHepStatus", "StdHepP4", "EvtVtx")
    }
    for name, lf in leaf.items():
        if not lf:
            raise SystemExit(f"error: leaf '{name}' missing from {path}")

    n_events = 0
    for i in range(t.GetEntries()):
        t.GetEntry(i)
        n = int(leaf["StdHepN"].GetValue(0))
        pdg = [int(leaf["StdHepPdg"].GetValue(k)) for k in range(n)]
        status = [int(leaf["StdHepStatus"].GetValue(k)) for k in range(n)]
        p4 = lambda k: [leaf["StdHepP4"].GetValue(4 * k + j) for j in range(4)]  # noqa: E731

        # Incoming neutrino: first initial-state entry with a neutrino PDG.
        nu = next((k for k in range(n) if status[k] == 0 and pdg[k] in NEUTRINOS), None)
        if nu is None:
            continue
        n_events += 1
        pdg_nu = pdg[nu]
        pnu = p4(nu)
        enu = pnu[3]
        hists["enu"].Fill(enu)
        if abs(pdg_nu) == 14:
            hists["enu_numu"].Fill(enu)

        # Vertex (SI meters in rootracker).
        x, y, z = (leaf["EvtVtx"].GetValue(j) for j in range(3))
        hists["vz"].Fill(z)
        if geo:
            node = geo.FindNode(x * 100, y * 100, z * 100)  # m -> cm
            name = node.GetVolume().GetMaterial().GetName() if node else "OUTSIDE"
            hists["mat"].Fill(name, 1.0)

        # Final state: multiplicity and the lepton (charged partner or
        # outgoing neutrino for NC).
        n_fs = 0
        lep = None
        lep_codes = (abs(pdg_nu), LEPTON_OF[abs(pdg_nu)])
        for k in range(n):
            if status[k] != 1:
                continue
            n_fs += 1
            if lep is None and abs(pdg[k]) in lep_codes:
                lep = k
        hists["mult"].Fill(n_fs)

        if lep is not None:
            pl = p4(lep)
            plep = math.sqrt(pl[0] ** 2 + pl[1] ** 2 + pl[2] ** 2)
            hists["plep"].Fill(plep)
            # q = p_nu - p_lep; Q2 = -q^2; W^2 = (q + p_N,rest)^2.
            q = [pnu[j] - pl[j] for j in range(4)]
            q2 = -(q[3] ** 2 - q[0] ** 2 - q[1] ** 2 - q[2] ** 2)
            hists["q2"].Fill(q2)
            w2 = (q[3] + M_NUCLEON) ** 2 - q[0] ** 2 - q[1] ** 2 - q[2] ** 2
            if w2 > 0:
                hists["w"].Fill(math.sqrt(w2))
    f.Close()
    return n_events


def align_material_bins(ha, hb):
    """Give both material histograms the same label set (union)."""
    labels = []
    for h in (ha, hb):
        for b in range(1, h.GetNbinsX() + 1):
            lab = h.GetXaxis().GetBinLabel(b)
            if lab and lab not in labels:
                labels.append(lab)
    out = []
    for h in (ha, hb):
        aligned = ROOT.TH1D(
            h.GetName() + "_al", h.GetTitle(), len(labels), 0, len(labels)
        )
        aligned.SetDirectory(ROOT.nullptr)
        aligned.Sumw2()
        for b, lab in enumerate(labels, start=1):
            aligned.GetXaxis().SetBinLabel(b, lab)
            src = h.GetXaxis().FindFixBin(lab)
            if src > 0 and h.GetXaxis().GetBinLabel(src) == lab:
                aligned.SetBinContent(b, h.GetBinContent(src))
                aligned.SetBinError(b, h.GetBinError(src))
        out.append(aligned)
    return out


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("sample_a")
    parser.add_argument("sample_b")
    parser.add_argument(
        "--gdml", default=None, help="geometry for vertex-material lookup"
    )
    parser.add_argument("--labels", nargs=2, default=["A", "B"])
    parser.add_argument(
        "-o", "--output", default=None, help="write histograms to this ROOT file"
    )
    parser.add_argument("--p-threshold", type=float, default=0.01)
    args = parser.parse_args()

    ROOT.gErrorIgnoreLevel = ROOT.kError
    geo = ROOT.TGeoManager.Import(args.gdml) if args.gdml else None
    if args.gdml and not geo:
        raise SystemExit(f"error: cannot import '{args.gdml}'")

    ha = booking(args.labels[0])
    hb = booking(args.labels[1])
    na = fill_from_rootracker(args.sample_a, ha, geo)
    nb = fill_from_rootracker(args.sample_b, hb, geo)
    print(f"{args.labels[0]}: {na} events   {args.labels[1]}: {nb} events\n")

    ha["mat"], hb["mat"] = align_material_bins(ha["mat"], hb["mat"])

    print(f"{'observable':<14} {'chi2/ndf':>10} {'p(chi2)':>10} {'p(KS)':>10}")
    worst = 1.0
    for key in ("enu", "enu_numu", "vz", "mat", "mult", "plep", "q2", "w"):
        a, b = ha[key], hb[key]
        if a.GetEntries() == 0 or b.GetEntries() == 0:
            print(f"{key:<14} {'—':>10} {'empty':>10} {'—':>10}")
            continue
        p_chi2 = a.Chi2Test(b, "UU NORM")
        chi2ndf = a.Chi2Test(b, "UU NORM CHI2/NDF")
        # KS needs an ordering — skip for the categorical material bins.
        p_ks = a.KolmogorovTest(b) if key != "mat" else float("nan")
        flag = (
            "  <-- LOW"
            if min(p_chi2, p_ks if not math.isnan(p_ks) else 1) < args.p_threshold
            else ""
        )
        ks_str = f"{p_ks:>10.3f}" if not math.isnan(p_ks) else f"{'n/a':>10}"
        print(f"{key:<14} {chi2ndf:>10.2f} {p_chi2:>10.3f} {ks_str}{flag}")
        worst = min(worst, p_chi2, p_ks if not math.isnan(p_ks) else 1)

    if args.output:
        out = ROOT.TFile.Open(args.output, "RECREATE")
        for h in list(ha.values()) + list(hb.values()):
            h.Write()
        out.Close()
        print(f"\nhistograms written to {args.output}")

    if worst < args.p_threshold:
        print(f"\nRESULT: at least one p-value below {args.p_threshold} — investigate.")
        return 1
    print(f"\nRESULT: samples statistically compatible (all p >= {args.p_threshold}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
