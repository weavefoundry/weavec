# Developer guide

## Toolchain

| Requirement        | Version                    | Notes                                                     |
| ------------------ | -------------------------- | --------------------------------------------------------- |
| CMake              | ≥ 3.24 (3.25 for presets)  |                                                           |
| Ninja              | any                        | Presets use Ninja; other generators work with `-G`.       |
| C++ compiler       | C++20                      | Clang ≥ 17, GCC ≥ 12, Apple Clang ≥ 15.                   |
| LLVM + Clang (dev) | 23.x recommended, ≥ 20     | Must include CMake packages (`LLVMConfig`, `ClangConfig`). |
| lit                | matching LLVM              | `pip install lit==23.1.0` or `brew install lit`.          |
| FileCheck/not/count| from LLVM                  | Shipped by Homebrew and apt.llvm.org's `llvm-N-tools`.    |
| clang-format/tidy  | same major as CI (23)      | Only for linting.                                         |

### macOS

```sh
brew install llvm ninja lit ccache
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"
```

The build also auto-detects the Homebrew `llvm` keg if `WEAVEC_LLVM_PREFIX`
is not set. Apple's system Clang can compile WeaveC, but the LLVM/Clang
*libraries* must come from Homebrew (Xcode does not ship them).

### Ubuntu / Debian

```sh
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 23 all
sudo apt-get install -y ninja-build ccache
pip install lit==23.1.0
export WEAVEC_LLVM_PREFIX=/usr/lib/llvm-23
export CC=clang-23 CXX=clang++-23
```

### Other LLVM installs

Point `CMAKE_PREFIX_PATH` (or `LLVM_DIR` and `Clang_DIR`) at any LLVM build
that installed its CMake packages, e.g. a from-source build with
`-DLLVM_ENABLE_PROJECTS=clang -DLLVM_INSTALL_UTILS=ON`.

## Building

```sh
cmake --preset dev             # Debug, assertions, compile_commands.json
cmake --build --preset dev
ctest --preset dev             # unit tests + lit
```

| Preset           | Purpose                                              |
| ---------------- | ---------------------------------------------------- |
| `dev`            | Debug build for day-to-day work                      |
| `dev-asan`       | Debug + AddressSanitizer + UBSan                     |
| `dev-tidy`       | Debug with clang-tidy running as part of compilation |
| `release`        | Optimised, LTO                                       |
| `relwithdebinfo` | Optimised with debug info                            |
| `ci-*`           | What CI runs: warnings are errors                    |

Useful cache variables (`-D...` or in `CMakeUserPresets.json`):

| Variable                   | Default | Effect                                              |
| -------------------------- | ------- | --------------------------------------------------- |
| `WEAVEC_BUILD_TESTS`       | ON      | Build unit and lit tests                            |
| `WEAVEC_WARNINGS_AS_ERRORS`| OFF     | `-Werror`                                           |
| `WEAVEC_SANITIZERS`        | ""      | `address;undefined`, `thread`, `memory`, `leak`     |
| `WEAVEC_ENABLE_LTO`        | OFF     | IPO for Release/RelWithDebInfo                      |
| `WEAVEC_ENABLE_CCACHE`     | ON      | Use ccache/sccache when found                       |
| `WEAVEC_ENABLE_CLANG_TIDY` | OFF     | Run clang-tidy during the build                     |
| `WEAVEC_LLVM_MIN_VERSION`  | 20.0    | Minimum accepted LLVM                               |

Build targets of note:

- `weavec` — the tool, in `build/<preset>/bin/`.
- `check-weavec` — build and run all tests.
- `check-weavec-unit`, `check-weavec-lit` — only one suite.

## Testing

### Unit tests (`unittests/`)

GoogleTest, one binary per library (`WeaveCCoreTests`, `WeaveCAnalysisTests`).
Core tests exercise the model directly; Analysis tests parse snippets with
`clang::tooling::buildASTFromCodeWithArgs` and collect diagnostics with
`core::DiagnosticCollector`. Run one with
`build/dev/unittests/WeaveCCoreTests --gtest_filter='Borrow*'`.

### Integration tests (`test/`)

lit + FileCheck; see [`test/README.md`](../test/README.md). Run a single test
with `lit -v build/dev/test/Analysis/use-after-free.c`. Every diagnostic change
should be covered by a lit test because they pin the exact user-visible output.

### Sanitizers

`cmake --workflow --preset ci-debug` builds with ASan+UBSan and runs
everything. Analysis code is the most likely place for lifetime bugs of our
own, so run this before submitting analysis changes.

## Formatting and linting

- `scripts/format.sh` formats C++ (clang-format) and CMake (cmake-format).
- `scripts/check-format.sh` / `scripts/check-cmake-format.sh` verify (CI runs
  these).
- `scripts/run-clang-tidy.sh [build-dir]` runs clang-tidy over the compile
  database; `cmake --preset dev-tidy` runs it as part of compilation.
- `pre-commit install` wires all of the above into `git commit`.

The `.clangd` config points at `build/compile_commands.json`; symlink your
preset's database there (`ln -s build/dev/compile_commands.json build/`) for
editor integration.

## Debugging the analysis

- `weavec file.c -- -Xclang -ast-dump` does *not* work (the tooling action
  replaces Clang's); use `clang -fsyntax-only -Xclang -ast-dump file.c`
  directly to inspect the AST.
- Run the tool under a debugger with `lldb -- build/dev/bin/weavec file.c --`.
- For lit failures, `lit -a` prints the full command and output; the test's
  working files are under `build/<preset>/test/<suite>/Output/`.

## Release checklist

1. Update `CHANGELOG.md` and move `[Unreleased]` to the new version.
2. Set `project(... VERSION x.y.z)` in `CMakeLists.txt` and configure with
   `-DWEAVEC_VERSION_SUFFIX=""`.
3. Tag `vX.Y.Z`; CI builds the release presets. `cpack -G TGZ` in the build
   directory produces a tarball.
