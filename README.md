# WeaveC

[Documentation](https://weavec.com) · [Getting started](https://weavec.com/getting-started/installation/) · [Diagnostic reference](https://weavec.com/reference/diagnostics/)

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

A mostly source-compatible C compiler that brings Rust-style memory safety to existing C code through inferred ownership and borrowing. It automatically proves memory safety where possible, requires lightweight annotations only when necessary, and isolates truly unsafe operations behind explicit `unsafe` boundaries. The goal is to let teams incrementally make large C codebases memory-safe without rewriting them in Rust or abandoning the C ecosystem.

WeaveC is built on Clang/LLVM rather than implementing a compiler from scratch, using Clang for parsing, semantic analysis, diagnostics, optimization, code generation, and platform support. WeaveC adds its own ownership inference, borrow checking, lifetime analysis, and memory-safety rules on top, allowing the project to focus on its core innovation while remaining compatible with the existing C toolchain and ecosystem.

WeaveC itself is written in modern C++, which provides the most direct and complete access to Clang/LLVM's APIs and infrastructure. The core ownership, borrowing, lifetime, and inference logic should be kept as modular as possible so it remains cleanly separated from the Clang integration layer and can potentially be reused or extended in the future.

> **Status:** WeaveC is early, v0.x software: flags, diagnostics and on-disk formats can change between minor versions. [RFC 0030](docs/rfcs/0030-prove-or-trap.md), *Prove or trap*, defines the current model and is being implemented. In that model, one ledger records an outcome for every safety facet of every memory operation (proven, checked at runtime, a definite violation, or unresolved or trusted with a stated reason), and `weavec-cc` turns spatial and null obligations it cannot prove into trapping runtime checks. Definite violations are errors, possible temporal bugs remain warnings, and checked mode (RFCs 0018–0029) is removed. The [roadmap](docs/roadmap.md) tracks progress.

## Quick look

```c
#include <stdint.h>
#include <stdlib.h>
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(size_t n);
size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);

struct node { int v; struct node *next; };
static void node_free(struct node *n) { free(n); }   // inferred: consumes n

int example(struct node *n) {
  struct node *m = n;
  node_free(m);
  return n->v;       // error: use of 'n' after it was freed [weavec::use-after-free]
}

int *escape(void) {
  int x = 0;
  return &x;         // error: returned pointer may outlive 'x' [weavec::lifetime-too-short]
}

struct node *WEAVEC_OWNED from_handle(uintptr_t h) {
  WEAVEC_UNSAFE { return (struct node *)h; }   // asserts ownership, at one greppable point
}

int handle(uintptr_t h) {
  struct node *r = (struct node *)h;             // r is raw: no one knows who owns it
  return r->v;       // error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
}
```

```
$ weavec example.c --
example.c:14:10: error: use of 'n' after it was freed [weavec::use-after-free]
   14 |   return n->v;
      |          ^
example.c:13:3: note: freed here (through 'm')
   13 |   node_free(m);
      |   ^
example.c:19:10: error: returned pointer may outlive 'x', which it points to [weavec::lifetime-too-short]
   19 |   return &x;
      |          ^
example.c:18:7: note: 'x' is declared here
   18 |   int x = 0;
      |       ^
example.c:28:10: error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
   28 |   return r->v;
      |          ^
example.c:27:20: note: 'r' is raw: cast from an integer here
   27 |   struct node *r = (struct node *)h;
      |                    ^
example.c:28:10: note: move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first
```

Annotations (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_UNSAFE`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_RETAINS`, `WEAVEC_RELEASES`, `WEAVEC_REFCOUNT`, `WEAVEC_OWNED_BY(f)`) expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## Using it as the compiler

`weavec-cc` is Clang's driver with WeaveC inside. Point a build at it and it compiles as `clang` would, analyses each file as it compiles it, and checks the whole program when it links:

```sh
$ CC=weavec-cc make
weavec-cc -c node.c -o node.o          # node.o and node.o.weavec (its summaries)
weavec-cc -c main.c -o main.o
weavec-cc node.o main.o -o prog        # reads the sidecars, analyses the program, then links
main.c:8:3: error: 'n' is freed twice [weavec::double-free]
    8 |   node_free(n);
      |   ^
main.c:7:3: note: previously freed here
    7 |   node_free(n);
      |   ^
1 error generated.
```

A bug inside one file is reported when that file is compiled; a bug that needs two files (`node_free` is defined in `node.c`) is reported when they are linked, and an error stops the link. Flags: `-fno-weavec` (compile only), `-fweavec-strict` (every call into unknown code is a raw operation), `-fno-weavec-link` (skip the link-time step), `-Wno-weavec-annotation-required`, `-Wno-error=weavec-use-after-free` (lower an error to a warning while migrating), `-Werror=weavec`.

The tooling form analyses a compilation database without building: `weavec --whole-program -p build/` (all sources) or `weavec --whole-program a.c b.c -- -Iinclude`. Without `--whole-program`, `weavec file.c --` checks one file as before.

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

## Releases

[GitHub Releases](https://github.com/weavefoundry/weavec/releases) is the
distribution channel for WeaveC. Initial 0.x releases provide
`weavec-X.Y.Z-source.tar.gz` and `SHA256SUMS`. They are early releases (see
*Status* above); APIs and on-disk formats can change between minor versions.

The first release creates `CHANGELOG.md` from Conventional Commits; later
releases regenerate it from the commit history.

Download both files into the same directory and verify the archive with
`shasum -a 256 -c SHA256SUMS` (or `sha256sum -c SHA256SUMS` on Linux).
Install the build requirements above, extract the archive, and run from its
`weavec-X.Y.Z` directory:

```sh
cmake --preset release -DWEAVEC_VERSION_SUFFIX=""
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
weavec --version
weavec-cc --version
```

Keep the LLVM/Clang installation used for the build available at runtime.
Prebuilt portable binaries and package-manager distribution are future work;
the current compiler records LLVM resource and executable paths at configure
time. The [release workflow](docs/development.md#releasing) describes version
selection, publication and retries.

## Repository layout

```
include/weavec/   Public C++ headers
  Core/           Ownership lattice, lifetimes, borrows, moves, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts; the checkers
  Frontend/       Clang FrontendAction / libTooling integration
lib/              Implementations, mirroring include/
tools/weavec/     The analysis tool (libTooling; --whole-program for a compilation database)
tools/weavec-cc/  The drop-in compiler driver (Clang's driver with WeaveC inside)
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
