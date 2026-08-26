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
# for shells that source this file without the package activation. GENIE's
# runtime configuration (tune XML, PDG tables) is resolved through this
# variable, as is genie-config at build time.
export GENIE="${GENIE:-$CONDA_PREFIX/share/genie}"
