<div align="center">

# WeaveC

**Memory safety for existing C code: every access guarded at run time, ownership bugs reported before it runs.**

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/weavefoundry/weavec?include_prereleases&sort=semver)](https://github.com/weavefoundry/weavec/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

[Documentation](https://weavec.com) · [Getting started](https://weavec.com/getting-started/installation/) · [Diagnostic reference](https://weavec.com/reference/diagnostics/) · [Releases](https://github.com/weavefoundry/weavec/releases)

</div>

WeaveC is a drop-in C compiler that makes the C code you already have stop at its first bad memory access, and an ownership and borrowing analysis that reports memory bugs before the program runs.

- **`weavec-cc` guards every memory access.** Before a load, a store or a C library call touches memory, a few inline instructions ask a small runtime linked into the program whether those bytes belong to a live object. An access past a heap block, a local or a global, a read of freed memory, or a use of a local after its scope has ended stops the program with a report that names the access. A guard is removed only where a simple local rule proves it redundant.
- **The analysis infers ownership.** It follows how pointers are allocated, shared, moved and released, and reports use after free, double free, dangling pointers and leaks, across files when asked. It is advisory: it never changes the compiled code, so a mistake in it can neither break a correct build nor remove a needed guard.

WeaveC is built on Clang and LLVM, which provide parsing, code generation and platform support. WeaveC adds the guard passes, the runtime and the analysis, in C++ libraries kept separate from the Clang integration.

> [!NOTE]
> WeaveC is early, v0.x software: flags, diagnostics, report formats and the ledger can change between minor versions. The design is [RFC 0035, *Guard by default*](docs/rfcs/0035-guard-by-default.md); [RFC 0036](docs/roadmap.md) will make inferred ownership contracts checked at function entry, so that they remove guards. The [roadmap](docs/roadmap.md) tracks progress.

## Highlights

- **A drop-in compiler.** `weavec-cc` is Clang's driver with WeaveC inside: point a build at it with `CC=weavec-cc` and it compiles as `clang` would. A correct program built with the default flags compiles, links and runs as it does with Clang, apart from run time; a false trap is a WeaveC bug.
- **Precise reports.** A failed guard prints the kind of failure, the source location and the address, and, for the heap, where the address lies relative to its block.
- **No ABI changes.** Guards are inline checks of a shadow memory; pointer representation and calling conventions are unchanged, and objects from other compilers link in.
- **Inference first.** The analysis needs no annotations to start; annotations from `weavec.h` state contracts where inference needs help, and they expand to nothing on other compilers.
- **An auditable ledger.** `-fweavec-ledger` lists every access the compiler saw and whether it kept its guard, and why not.

## Quick look

A read past a heap block, built with `weavec-cc` and run:

```c
struct vec { int *data; size_t len; };
int get(struct vec *v, size_t i) { return v->data[i]; }   // v->data holds 4 ints
```

```console
$ weavec-cc vec.c -o vec
$ ./vec 4
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xbd80000010
weavec: 0xbd80000010 is 0 bytes after the 16-byte heap object at 0xbd80000000
```

The program stops with `SIGTRAP` (or `SIGILL` on x86-64) at the access, instead of reading past the block. The same guards stop a read of a freed block (`heap-use-after-free`), an overflow of a local or a global (`stack-buffer-overflow`, `global-buffer-overflow`), a use of a local after its scope (`stack-use-after-scope`), an index past an array's declared bound (`index-out-of-bounds`) and a double or invalid `free`.

The analysis finds bugs without running the program:

```c
#include <stdlib.h>

struct node { int v; struct node *next; };
static void node_free(struct node *n) { free(n); }   // inferred: consumes n

int example(struct node *n) {
  struct node *m = n;
  node_free(m);
  return n->v;       // error: freed on every path
}

int maybe(struct node *n, int done) {
  if (done)
    node_free(n);
  return n->v;       // warning: freed on some paths
}

int *escape(void) {
  int x = 0;
  return &x;         // error: the pointer outlives x
}
```

```console
$ weavec example.c --
example.c:20:11: warning: address of stack memory associated with local variable 'x' returned [-Wreturn-stack-address]
   20 |   return &x;         // error: the pointer outlives x
      |           ^
example.c:9:10: error: use of 'n' after it was freed [weavec::use-after-free]
    9 |   return n->v;       // error: freed on every path
      |          ^
example.c:8:3: note: freed here (through 'm')
    8 |   node_free(m);
      |   ^
example.c:15:10: warning: use of 'n' after it may have been freed [weavec::use-after-free]
   15 |   return n->v;       // warning: freed on some paths
      |          ^
example.c:14:5: note: freed here on some paths
   14 |     node_free(n);
      |     ^
example.c:20:10: error: returned pointer may outlive 'x', which it points to [weavec::lifetime-too-short]
   20 |   return &x;         // error: the pointer outlives x
      |          ^
example.c:19:7: note: 'x' is declared here
   19 |   int x = 0;
      |       ^
weavec: example.c: 9 sites: 5 proven, 2 not proven, 2 violations, 0 trusted; 2 errors, 1 warning
2 warnings and 2 errors generated.
Error while processing example.c.
```

Definite bugs are errors; a use-after-free on only some paths is a warning. (The first warning is Clang's own.) The last line summarises the analysis of the file's memory operations (*sites*). `weavec-cc -fweavec-diagnose` prints the same findings as warnings during a build.

## How it works

The runtime keeps one *shadow* byte for every 16 bytes of memory, saying which of them belong to a live object, and replaces the program's allocator: every heap object sits in a slot with at least one byte after it that belongs to no object, and released blocks wait in a quarantine (16 MiB by default) before they are reused. `weavec-cc` lays out the locals a guard can reach in one frame with *redzones* between them, gives the file's globals redzones, and zero-initialises locals.

Four LLVM passes then guard the code. The first puts a guard before every load, store, atomic operation and memory intrinsic, and before every memory argument of a call in the [library table](lib/Core/LibrarySpec.txt), before the optimiser runs, so the optimiser cannot exploit the bug before the guard sees it. The others remove the guards a local rule proves redundant (an access inside a local or a global at a known offset, an access a dominating guard on the same pointer already covered with nothing between that may free, nearby accesses merged into one guard, a loop whose whole range is checked once before it runs) and expand the rest into a three-instruction check of the shadow that calls the runtime only when a byte is not zero. Array indexes are also checked against their declared bounds. `-fweavec-checks=verify` keeps every removed guard as a monitor, so a wrong removal is caught in testing.

The analysis runs separately, in the `weavec` tool or under `-fweavec-diagnose`: ownership inference, borrow and lifetime checking, null and bounds reasoning over an abstract heap, and function summaries that let a caller see what a callee does, across files with `weavec --whole-program`.

### Annotations

Where the analysis needs help, typically at public interfaces, annotations from `weavec.h` state the contract: `WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(q)`, `WEAVEC_STRING`, `WEAVEC_ASSUME(e)` and the reference-counting forms. They are inputs to the analysis and expand to nothing on other compilers. One changes the compiled code: `WEAVEC_UNSAFE` on a function or block leaves its accesses unguarded, for code that reads past an object on purpose. See [docs/annotations.md](docs/annotations.md).

## Using it as the compiler

Point a build at `weavec-cc`:

```sh
make CC=weavec-cc
cmake -S . -B build -DCMAKE_C_COMPILER=weavec-cc
```

Every link adds the runtime (`libweavec_rt.a`) and its allocator (`libweavec_alloc.a`). On Darwin one runtime serves the whole process, and its allocator also serves the C library's own allocations (`strdup`, `getline`, `asprintf`). `weavec-cc` builds without the runtime, and without guards, under a sanitizer that replaces the allocator, with `-ffreestanding` or `-nostdlib`, and on targets other than 64-bit Darwin and Linux, and prints a note at the link. A program that defines `malloc` itself keeps its allocator, and its heap is untracked.

`-fweavec-checks=` selects what a failed guard does:

| Mode | A failed guard | Removed guards | Zero-init | Runtime | Guarantee |
| --- | --- | --- | --- | --- | --- |
| `trap` (default) | prints a report and traps | removed | on | linked | yes |
| `report` | prints a report once per site and continues | removed | on | linked | only with `WEAVEC_RT_ABORT=1` |
| `verify` | prints a report and traps | kept as monitors (`weavec.proven`) | on | linked | yes |
| `none` | nothing is guarded: the object file is what Clang would produce | | off | not linked | no |

Other useful flags and variables:

| Flag | Effect |
| --- | --- |
| `-fno-weavec` | Compile as plain Clang. |
| `-fno-weavec-zero-init` | Do not zero-initialise locals. |
| `-fweavec-ledger=<dir>/`, `-fweavec-summary` | Write the enforcement ledger per unit; print `weavec: file.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded`. |
| `-fweavec-diagnose` | Run the analysis and print its findings as warnings (`-Werror=weavec` makes them errors). |
| `WEAVEC_RT_STATS=1` | Print the runtime's counters when the program exits. |
| `WEAVEC_RT_REPORT_LOG=<path>` | Write reports to a file instead of stderr. |
| `WEAVEC_RT_QUARANTINE=<bytes>` | Change the quarantine's size. |

`weavec-cc --help-weavec` lists every option; the [command-line reference](docs/pages/reference/cli.md) documents both tools in full.

### Analysis without building

The `weavec` tool analyses without producing binaries. `weavec file.c --` checks one file; `weavec --whole-program -p build/` analyses every source in a compilation database as one program, and `weavec --whole-program a.c b.c -- -Iinclude` the given sources. A definite finding is an error and the tool exits with status 1, so it can gate CI.

## What WeaveC guarantees

For a translation unit compiled by `weavec-cc` in an enforcing mode (`trap` or `verify`), with the runtime linked, every memory access outside a `WEAVEC_UNSAFE` region satisfies:

- **(A) Accesses.** A load, store, atomic operation or memory intrinsic executes only if every byte it touches is *addressable* when it runs, and so does each memory argument of a call to a function in the library table; otherwise the program traps first.
- **(B) Bounds.** An index into an array whose bound its type declares (other than a trailing array of a struct) is within that bound, or the program traps.
- **(R) Releases.** A release through the runtime's allocator releases null or the start of a live heap object, or the program stops.

A byte is not addressable when it lies after a heap object in its slot, in a released and quarantined block, in a redzone of a tracked local or global, in a local outside its scope, or in the lowest 64 KiB. Every other byte is addressable. So, deliberately, these pass: an access that jumps over a redzone into another live object; a use of a freed block after the quarantine recycled it; a use of a local after its function returned; an overflow from one field of a struct into the next by pointer arithmetic or a library call; accesses to memory the runtime does not track (other allocators, `mmap`, string literals); wide-string library arguments; and code that `weavec-cc` did not compile. The guarantee assumes that untracked memory is accessed correctly, that library functions access only what the library table says, that there are no data races between a guard and its access, and that the program does not write the runtime's shadow. The analysis is not part of it.

The full statement is in [the guarantees reference](docs/pages/reference/guarantees.md) and [RFC 0035, *Soundness*](docs/rfcs/0035-guard-by-default.md#soundness).

## Status

RFC 0035 replaced the analysis-driven enforcement of RFCs 0030–0034 with guards on every access. Measured on its final tree (the RFC's *Measured gates* has the details): ten C projects WeaveC was never tuned on (git, tcpdump, libtiff, BearSSL, YARA, gawk, libarchive, Tcl, json-c, MicroPython) build with their own build systems and `CC=weavec-cc` and pass their test suites (json-c with its own option against a deliberate over-read, MicroPython with a one-line change that lets Clang see the `no_sanitize_address` on its stack scan, as ASan needs too); the default build stops more of a set of 64 blind bug programs than AddressSanitizer (60 against 58), with no false stop on their fixed twins; and every measured workload runs at or below ASan's cost, typically 1.3–2.3 times the reference compiler's (Lua 2.2x, zlib 1.3x, cJSON 1.8x, against ASan's 2.5x, 1.4x and 3.1x). A program starts about 1.2 ms later than without WeaveC. Interpreters and loops over pointers whose exit depends on the data pay the most: measure on your own workload. Ownership enforcement returns in RFC 0036, as contracts checked at function entry.

## Building

**Requirements:** CMake ≥ 3.25, Ninja, a C++20 compiler, and an LLVM/Clang development install (23.x recommended; ≥ 20 supported). The tests also need `lit` and LLVM's `FileCheck`.

```sh
# macOS
brew install llvm ninja lit
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"

# Ubuntu / Debian
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 23 all
sudo apt-get install -y ninja-build && pip install lit
export WEAVEC_LLVM_PREFIX=/usr/lib/llvm-23

# All platforms
cmake --preset dev          # configure into build/dev
cmake --build --preset dev  # build
ctest --preset dev          # unit, runtime, lit and test/cases suites, in parallel
```

Other presets are `dev-asan`, `dev-tidy`, `release` and `relwithdebinfo`; see [`CMakePresets.json`](CMakePresets.json) for the full list and the [developer guide](docs/development.md) for testing, debugging and tooling.

## Installing a release

Releases are published on [GitHub Releases](https://github.com/weavefoundry/weavec/releases). Each provides a source archive, `weavec-X.Y.Z-source.tar.gz`, and a `SHA256SUMS` file; release notes are in [CHANGELOG.md](CHANGELOG.md).

1. Download both files into the same directory and verify the archive with `shasum -a 256 -c SHA256SUMS` (or `sha256sum -c SHA256SUMS` on Linux).
2. Install the [build requirements](#building).
3. Extract the archive and, from its `weavec-X.Y.Z` directory, build, test and install:

```sh
cmake --preset release -DWEAVEC_VERSION_SUFFIX=""
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
weavec --version
weavec-cc --version
```

Keep the LLVM/Clang installation used for the build available at run time: the compiler records LLVM's resource and executable paths at configure time. Prebuilt portable binaries and package-manager distribution are planned. The [release workflow](docs/development.md#releasing) describes how versions are selected and published.

## Documentation

| Topic | Where |
| --- | --- |
| Installation, first check, guides | [weavec.com](https://weavec.com) |
| Annotations and diagnostics | [docs/annotations.md](docs/annotations.md) |
| Command-line reference | [docs/pages/reference/cli.md](docs/pages/reference/cli.md) |
| Guarantees and scope | [docs/pages/reference/guarantees.md](docs/pages/reference/guarantees.md) |
| Architecture | [docs/architecture.md](docs/architecture.md) |
| Design decisions (RFCs) | [docs/rfcs/](docs/rfcs/) |
| Developer guide | [docs/development.md](docs/development.md) |
| Roadmap | [docs/roadmap.md](docs/roadmap.md) |

## Repository layout

```
include/weavec/   Public C++ headers
  Core/           The analysis's model: ownership lattice, pointer kinds, the abstract heap,
                  summaries, the analysis ledger, the library table, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts: sites, kinds, slots, the object engine behind one seam
  Frontend/       Clang and LLVM integration: the guard passes, unsafe regions, the enforcement
                  ledger, the analysis action, whole-program analysis, the compiler driver
lib/              Implementations, mirroring include/ (lib/Core/LibrarySpec.txt is the library table)
runtime/          The C runtime: the shadow memory, the allocator, guards' slow paths, checked
                  library calls, reports
tools/weavec/     The analysis tool (libTooling; --whole-program for a compilation database)
tools/weavec-cc/  The drop-in compiler driver (Clang's driver with WeaveC inside)
resources/        weavec.h, the C-facing annotation header (installed to lib/weavec/include)
unittests/        GoogleTest unit tests
test/             lit + FileCheck integration tests (test/Guards pins the passes and the runtime)
  cases/          Executable C cases by feature, with markers, run by scripts/run-cases.py
  corpus/         Real projects pinned by SHA, run by scripts/corpus-gate.py
scripts/          Test runners, the corpus gate, hygiene and release tooling
docs/             Architecture, RFCs (docs/rfcs/), roadmap, and the weavec.com site
cmake/            Build-system modules
```

The layering rule is strict: `Core` must not include anything from `clang/` or `llvm/`. `Analysis` is the only layer that knows Core and Clang's AST, and only the object engine behind the `SafetyEngine` seam (`lib/Analysis/Engine*.cpp`) sees its own internals. The runtime is C and includes no WeaveC header but its own. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md) first. Report security issues privately, as described in [SECURITY.md](SECURITY.md).

## License

WeaveC is licensed under the Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
