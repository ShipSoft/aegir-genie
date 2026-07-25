# Changelog

All notable changes to this project will be documented in this file.

## [0.2.0] - 2026-07-25

### Features

- Share the geometry and its Geant4 thread with aegir's geant4_module
- Refuse spline files generated for a different tune

### Bug fixes

- Default to the packaged splines' tune G18_02a_02_11b

### Documentation

- Document the in-process genie+geant4 chain limitation

### Build

- Require the released shipgeometryservice >=0.3
- Refresh pixi.lock for shipgeometryservice 0.3.0
## [0.1.0] - 2026-07-16

### Features

- Vendor aegir's generator helper headers
- Add embedded GENIE source with SHiP flux driver
- Add gevgen_ship standalone generator app
- Support GENIE GSimple flux files
- Add statistical sample comparison script
- Add GSimple flux-file shuffle macro
- Analyze the GeoModel geometry directly, replacing the GDML path

### Bug fixes

- Make plugin and gevgen_ship event sequences identical
- Scan max path lengths with the flux, not the box scanner
- Weight the delivered-POT accounting by ray weights
- Validate top_volume against the imported geometry
- Fail loudly on flux PDG codes unknown to GENIE
- Derive per-event RNG streams injectively from (seed, event)
- Harden geometry resolution and analyzer cleanup paths

### Refactor

- Extract shared GENIE driver assembly
- Small cleanups and C++23 modernizations
- Unify the ordered-generation paths in genie_source

### Documentation

- Validate against gevgen_fnal and the full phlex chain
- Record geometry-analyzer validation results

### Styling

- Clang-format

### Testing

- Report unexpected exceptions instead of aborting

### Miscellaneous

- Scaffold GPL-3.0 plugin repository
- Relicense sources to LGPL-3.0-or-later

### Build

- Collapse to a single pixi environment
- Consume genie from prefix.dev/ship
- Add lint, CI, Doxygen and release tooling
- Compile shared sources once via an OBJECT library
- Depend on the genie-splines-ship package
- Require the released shipgeometryservice >=0.2
