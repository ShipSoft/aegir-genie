#!/bin/bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pixi activation script for aegir-genie.
# Sourced automatically by `pixi run` / `pixi shell`.

export AEGIR_GENIE_ROOT="$PIXI_PROJECT_ROOT"

# Locally built plugins first, then installed plugins from the pixi env
# (which is also where an installed aegir package would put its plugins).
export PHLEX_PLUGIN_PATH="$PIXI_PROJECT_ROOT/build:${CONDA_PREFIX}/lib${PHLEX_PLUGIN_PATH:+:$PHLEX_PLUGIN_PATH}"
export LD_LIBRARY_PATH="$PIXI_PROJECT_ROOT/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# GENIE lives in the separate `genie` pixi environment (see pixi.toml for the
# root_cxx_standard conflict that forces this). Its own activation script
# would export GENIE=$CONDA_PREFIX/share/genie, but tasks run in the default
# environment, so point GENIE at the genie env prefix explicitly. GENIE's
# runtime configuration (tune XML, PDG tables) is resolved through this
# variable, as is genie-config at build time.
export GENIE="${GENIE:-$PIXI_PROJECT_ROOT/.pixi/envs/genie/share/genie}"

# Deliberately NOT adding .pixi/envs/genie/lib to LD_LIBRARY_PATH: that prefix
# also holds the C++20 ROOT build, which must not shadow the default
# environment's C++23 ROOT. The plugin finds the GENIE libraries through its
# RPATH instead.
