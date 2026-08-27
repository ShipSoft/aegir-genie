#!/bin/bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Pixi activation script for aegir-genie.
# Sourced automatically by `pixi run` / `pixi shell`.

export AEGIR_GENIE_ROOT="$PIXI_PROJECT_ROOT"

# Locally built plugins first, then installed plugins from the pixi env
# (which is also where an installed aegir package would put its plugins).
export PHLEX_PLUGIN_PATH="$PIXI_PROJECT_ROOT/build:${CONDA_PREFIX}/lib${PHLEX_PLUGIN_PATH:+:$PHLEX_PLUGIN_PATH}"
export LD_LIBRARY_PATH="$PIXI_PROJECT_ROOT/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# GENIE lives in the default environment (its C++23-ROOT build coexists with
# phlex, see pixi.toml); the genie conda package's own activation script
# normally exports GENIE=$CONDA_PREFIX/share/genie already. Keep a fallback
# for shells that source this file without the package activation, but only
# when the directory exists — otherwise warn and leave GENIE unset rather
# than exporting a bogus path (this file is sourced, so a hard failure would
# break activation; CMakeLists.txt and the GENIE runtime fail loudly on
# their own). GENIE's runtime configuration (tune XML, PDG tables) is
# resolved through this variable, as is genie-config at build time.
if [ -z "${GENIE:-}" ]; then
    if [ -d "${CONDA_PREFIX:-/nonexistent}/share/genie" ]; then
        export GENIE="$CONDA_PREFIX/share/genie"
    else
        echo "activate.sh: \$GENIE not set and \$CONDA_PREFIX/share/genie not found" >&2
    fi
fi
