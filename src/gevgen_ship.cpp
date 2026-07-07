// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// gevgen_ship.cpp — standalone SHiP neutrino event generator
//
// Command-line counterpart of the genie_source phlex plugin, sharing the
// exact same driver assembly (genie_driver_setup.hpp): SHiP flux ntuple ×
// cross-section splines × TGeo geometry through a genie::GMCJDriver, with
// the same per-event Philox reseeding — identical config and seed produce
// the same event sequence in both.
//
// Unlike the plugin, output is native GENIE GHEP (genie::NtpWriter), so the
// file converts with `gntpc -f rootracker` and reads back through aegir's
// genie_reader_source — the apples-to-apples validation path between the
// embedded source and the file-reader chain.

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Framework/EventGen/EventRecord.h"
#include "Framework/EventGen/GMCJDriver.h"
#include "Framework/Ntuple/NtpMCFormat.h"
#include "Framework/Ntuple/NtpWriter.h"
#include "Framework/ParticleData/PDGCodeList.h"
#include "Tools/Flux/GFluxExposureI.h"
#include "genie_config.hpp"
#include "genie_driver_setup.hpp"
#include "ship_flux_driver.hpp"

namespace {

struct CliOptions {
  aegir::GenieSourceConfig cfg;
  long n_events = -1;
  std::string output;  // .root file name, or NtpWriter filename prefix
  long run = 0;
  bool dry_run = false;
};

constexpr char kUsage[] =
    R"(gevgen_ship — SHiP neutrino event generator (GENIE, GHEP output)

Usage:
  gevgen_ship -f FLUX -g GDML -x SPLINES -n EVENTS [options]

Required:
  -f, --flux FILE           neutrino flux file (see --flux-format)
  -g, --geometry FILE       detector GDML (imported with TGeoManager::Import)
  -x, --splines FILE        cross-section splines (gmkspl XML output)
  -n, --events N            number of events to generate

Options:
      --flux-format FMT     'ship' (SHiP flux ntuple, schema v1) or
                            'gsimple' (GENIE GSimple flux, the gevgen_fnal
                            input format; root:// URLs supported)
                            [default: ship]
  -o, --output NAME         output GHEP file. NAME ending in .root is used
                            verbatim; anything else is a filename prefix
                            (NAME.RUN.ghep.root)   [default: gevgen_ship]
  -t, --tune TUNE           GENIE tune; must match the splines
                            [default: G18_02a_00_000]
  -v, --top-volume NAME     TGeo top volume        [default: World]
  -s, --seed SEED           base seed; each event derives its own via Philox
                            [default: 20260706]
  -m, --max-path-lengths FILE
                            XML cache for the max-path-lengths geometry scan:
                            loaded if it exists, computed and saved otherwise
      --run N               run number written to the GHEP tree [default: 0]
      --dry-run             stop after GMCJDriver::Configure() (checks config,
                            tune, splines, geometry and flux without
                            generating events)
  -h, --help                print this message

The generated sample carries its normalisation: the summary line reports the
flux rays used, GMCJDriver's global probability scale and the delivered
POT equivalent (fraction of the flux file used x its POT).
)";

[[noreturn]] void fail(std::string const& msg) {
  std::cerr << "gevgen_ship: " << msg << "\n\nRun gevgen_ship --help.\n";
  std::exit(1);
}

CliOptions parse_args(int argc, char** argv) {
  CliOptions opt;
  std::vector<std::string> const args(argv + 1, argv + argc);

  auto value = [&args](std::size_t& i, std::string const& flag) {
    if (i + 1 >= args.size()) fail("missing value for " + flag);
    return args[++i];
  };
  auto long_value = [](std::string const& s, long& out) {
    std::istringstream in{s};
    return static_cast<bool>(in >> out) && in.eof();
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    auto const& a = args[i];
    if (a == "-h" || a == "--help") {
      std::cout << kUsage;
      std::exit(0);
    } else if (a == "-f" || a == "--flux") {
      opt.cfg.flux_file = value(i, a);
    } else if (a == "--flux-format") {
      opt.cfg.flux_format = value(i, a);
    } else if (a == "-g" || a == "--geometry") {
      opt.cfg.gdml_file = value(i, a);
    } else if (a == "-x" || a == "--splines") {
      opt.cfg.spline_file = value(i, a);
    } else if (a == "-n" || a == "--events") {
      if (!long_value(value(i, a), opt.n_events) || opt.n_events <= 0)
        fail("--events expects a positive integer, got '" + args[i] + "'");
    } else if (a == "-o" || a == "--output") {
      opt.output = value(i, a);
    } else if (a == "-t" || a == "--tune") {
      opt.cfg.tune = value(i, a);
    } else if (a == "-v" || a == "--top-volume") {
      opt.cfg.top_volume = value(i, a);
    } else if (a == "-s" || a == "--seed") {
      if (!long_value(value(i, a), opt.cfg.seed))
        fail("--seed expects an integer, got '" + args[i] + "'");
    } else if (a == "-m" || a == "--max-path-lengths") {
      opt.cfg.max_path_lengths_file = value(i, a);
    } else if (a == "--run") {
      if (!long_value(value(i, a), opt.run) || opt.run < 0)
        fail("--run expects a non-negative integer, got '" + args[i] + "'");
    } else if (a == "--dry-run") {
      opt.dry_run = true;
    } else {
      fail("unknown argument '" + a + "'");
    }
  }

  if (opt.n_events < 0 && !opt.dry_run)
    fail("--events is required (or use --dry-run)");
  return opt;
}

// Flux description for logs: rays/POT of the whole file where the driver
// exposes them (ship schema v1); flavour list and max energy come from the
// generic GFluxI interface.
std::string describe_flux(aegir::GenieDriverBundle const& bundle) {
  std::ostringstream out;
  if (auto const* ship =
          dynamic_cast<aegir::ShipFluxDriver*>(bundle.flux.get())) {
    out << ship->entries() << " rays, " << ship->pot() << " POT, ";
  }
  out << "max energy " << bundle.flux->MaxEnergy() << " GeV, flavours [";
  auto const& pdgs = bundle.flux->FluxParticles();
  for (std::size_t i = 0; i < pdgs.size(); ++i)
    out << (i ? ", " : "") << pdgs[i];
  out << "]";
  return out.str();
}

int run(CliOptions const& opt) {
  // Same assembly as the plugin: config validation (clear errors for missing
  // files), tune, splines, geometry, flux, Configure().
  auto bundle = aegir::make_genie_driver(opt.cfg, "gevgen_ship");

  if (opt.dry_run) {
    std::cout << "gevgen_ship: dry run OK — tune '" << opt.cfg.tune
              << "', splines '" << opt.cfg.spline_file << "', flux ("
              << opt.cfg.flux_format << ") " << describe_flux(bundle)
              << ", GlobProbScale = " << bundle.driver->GlobProbScale() << '\n';
    return 0;
  }

  genie::NtpWriter writer(genie::kNFGHEP, opt.run, opt.cfg.seed);
  if (!opt.output.empty()) {
    if (opt.output.ends_with(".root"))
      writer.CustomizeFilename(opt.output);
    else
      writer.CustomizeFilenamePrefix(opt.output);
  } else {
    writer.CustomizeFilenamePrefix("gevgen_ship");
  }
  writer.Initialize();

  for (long i = 0; i < opt.n_events; ++i) {
    aegir::reseed_event(opt.cfg.seed, static_cast<std::uint32_t>(i));

    std::unique_ptr<genie::EventRecord> event{bundle.driver->GenerateEvent()};
    if (!event) {
      // The flux drivers cycle, so exhaustion "cannot happen" — but if GENIE
      // still comes back empty-handed, say what the flux looked like.
      auto const* exposure = bundle.exposure();
      std::cerr << "gevgen_ship: GMCJDriver::GenerateEvent returned no event "
                   "at event "
                << i << " — flux file '" << opt.cfg.flux_file << "' ("
                << describe_flux(bundle) << "; cycled, "
                << (exposure ? exposure->NFluxNeutrinos() : -1)
                << " rays thrown so far). Check the flux/geometry overlap "
                   "and the spline coverage.\n";
      return 1;
    }
    aegir::trace_event(static_cast<std::uint32_t>(i), *event, *bundle.flux);
    writer.AddEventRecord(static_cast<int>(i), event.get());

    if ((i + 1) % 100 == 0 || i + 1 == opt.n_events)
      std::cout << "gevgen_ship: generated " << (i + 1) << " / " << opt.n_events
                << " events\n";
  }

  writer.Save();

  // Normalisation summary: with ForceSingleProbScale the sample is
  // unweighted and NFluxNeutrinos * GlobProbScale relates events to flux
  // rays; the POT equivalent comes from the driver's GFluxExposureI
  // implementation (ship: POT x fraction of the file consumed; gsimple:
  // accumulated effective POT per unweighted ray, as in gevgen_fnal).
  auto const* exposure = bundle.exposure();
  std::cout << "gevgen_ship: done — " << opt.n_events << " events\n"
            << "  flux rays used:      "
            << (exposure ? exposure->NFluxNeutrinos() : -1)
            << " (file: " << describe_flux(bundle) << ")\n"
            << "  global prob. scale:  " << bundle.driver->GlobProbScale()
            << '\n'
            << "  POT equivalent:      "
            << (exposure ? exposure->GetTotalExposure() : 0.0) << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto const opt = parse_args(argc, argv);
  try {
    return run(opt);
  } catch (std::exception const& e) {
    // Config/setup errors already carry the "gevgen_ship:" context.
    std::string const what = e.what();
    if (what.starts_with("gevgen_ship:"))
      std::cerr << what << '\n';
    else
      std::cerr << "gevgen_ship: " << what << '\n';
    return 1;
  }
}
