# Contributing to aegir-genie

Thank you for your interest in contributing to aegir-genie! As part of the SHiP Collaboration, we follow a set of standards to ensure code quality and maintainability.

## Development Workflow

1. **Fork and Clone**: Create a fork of the repository and clone it locally.
2. **Environment**: Install [pixi](https://pixi.sh) — it provisions all build and runtime dependencies (ROOT, GENIE, Phlex, SHiPDataModel, …) from `conda-forge` and [`prefix.dev/ship`](https://prefix.dev/channels/ship). Then `pixi install` once to materialise the environment.
3. **Pre-commit Hooks**: We use [`prek`](https://github.com/j178/prek) (a drop-in `pre-commit` replacement) to enforce coding standards. The hook tools come from the pixi `lint` environment, so versions are tracked in `pixi.lock` and run identically everywhere. Install the hooks once:
   ```bash
   pixi run install-hooks
   ```
   Run all hooks manually at any time with `pixi run lint`.
4. **Branching**: Create a feature branch for your changes.
5. **Coding Standards**:
   - Follow the existing C++ style (enforced by `clang-format` and `cpplint`).
   - Use `ruff` for Python script formatting.
   - CMake files are formatted with `gersemi`.
   - Ensure all files have the correct SPDX license headers (REUSE compliant).
6. **Commits**: We follow [Conventional Commits](https://www.conventionalcommits.org/) (validated by `commitizen`). This helps in automated changelog generation.
   - `feat: ...` for new features
   - `fix: ...` for bug fixes
   - `docs: ...` for documentation changes
   - `style: ...` for formatting
   - `refactor: ...` for code refactoring
7. **Testing**:
   - Build and run the tests inside the pixi environment:
     ```bash
     pixi run build
     pixi run test
     ```
     Or `pixi shell` first if you prefer an interactive session.
   - The standalone `gevgen_ship` generator and the `genie_source` phlex plugin
     assemble the identical GENIE driver; validate changes against both paths
     (see `README.md`).
8. **Submission**: Open a Pull Request against the `main` branch. Ensure the CI passes.

## Coding Style

- **C++**: We use C++23. Style is defined in `.clang-format`.
- **Python**: Follow PEP 8 (enforced by `ruff`).
- **Configuration**: Workflows are defined using [Jsonnet](https://jsonnet.org/).

## Licensing

The SHiP-authored source in this repository is licensed **LGPL-3.0-or-later**, so
it stays reusable and can move to a shared package. Because the plugin links the
**GPL** GENIE library, the *combined program* is distributed under
**GPL-3.0-or-later** (LGPL-3.0-or-later is compatible with this). All
contributions must be compatible with LGPL-3.0-or-later, and each new file must
include an SPDX identifier and copyright notice.
