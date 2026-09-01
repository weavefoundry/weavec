# WeaveC

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

A mostly source-compatible C compiler that brings Rust-style memory safety to existing C code through inferred ownership and borrowing. It automatically proves memory safety where possible, requires lightweight annotations only when necessary, and isolates truly unsafe operations behind explicit `unsafe` boundaries. The goal is to let teams incrementally make large C codebases memory-safe without rewriting them in Rust or abandoning the C ecosystem.

WeaveC is built on Clang/LLVM rather than implementing a compiler from scratch, using Clang for parsing, semantic analysis, diagnostics, optimization, code generation, and platform support. WeaveC adds its own ownership inference, borrow checking, lifetime analysis, and memory-safety rules on top, allowing the project to focus on its core innovation while remaining compatible with the existing C toolchain and ecosystem.

WeaveC itself is written in modern C++, which provides the most direct and complete access to Clang/LLVM's APIs and infrastructure. The core ownership, borrowing, lifetime, and inference logic should be kept as modular as possible so it remains cleanly separated from the Clang integration layer and can potentially be reused or extended in the future.

> **Status:** early scaffolding. The pipeline runs end to end (parse → analyse → diagnose) with a deliberately small local-ownership checker; the real inference engine is being built on top. See [docs/roadmap.md](docs/roadmap.md).

## Quick look

```c
#include <stdlib.h>
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(size_t n);
size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);

void example(void) {
  int *p = malloc(sizeof *p);
  free(p);
  *p = 1;            // error: use of 'p' after it was freed [weavec::use-after-free]
}
```

```
$ weavec example.c --
example.c:10:4: error: use of 'p' after it was freed [weavec::use-after-free]
   10 |   *p = 1;
      |    ^
example.c:9:3: note: freed here
    9 |   free(p);
      |   ^
```

Annotations (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_UNSAFE`) expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## Building

Requirements: CMake ≥ 3.24, Ninja, a C++20 compiler, and an LLVM/Clang development install (23.x recommended; ≥ 20 supported). Tests additionally need `lit` and LLVM's `FileCheck`.

```sh
# macOS
brew install llvm ninja lit
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"

# Ubuntu / Debian
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 23 all
sudo apt-get install -y ninja-build && pip install lit
export WEAVEC_LLVM_PREFIX=/usr/lib/llvm-23

# Everyone
cmake --preset dev          # configure into build/dev
cmake --build --preset dev  # build
ctest --preset dev          # run unit + integration tests
```

Other presets: `dev-asan`, `dev-tidy`, `release`, `relwithdebinfo`. The full list is in [`CMakePresets.json`](CMakePresets.json); the developer guide is [docs/development.md](docs/development.md).

## Repository layout

```
include/weavec/   Public C++ headers
  Core/           Ownership lattice, lifetimes, borrows, moves, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts; the checkers
  Frontend/       Clang FrontendAction / libTooling integration
lib/              Implementations, mirroring include/
tools/weavec/     The command-line tool
resources/        weavec.h, the C-facing annotation header (installed to lib/weavec/include)
unittests/        GoogleTest unit tests
test/             lit + FileCheck integration tests
docs/             Architecture, RFCs (docs/rfcs/), roadmap
cmake/            Build-system modules
```

The layering rule is strict: `Core` must not include anything from `clang/` or `llvm/`. `Analysis` is the only layer that knows about both worlds. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) first. Security issues should be reported privately as described in [SECURITY.md](SECURITY.md).

## License

Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
