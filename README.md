# WeaveC

[Documentation](https://weavec.com) · [Getting started](https://weavec.com/getting-started/installation/) · [Diagnostic reference](https://weavec.com/reference/diagnostics/)

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

A mostly source-compatible C compiler that brings Rust-style memory safety to existing C code through inferred ownership and borrowing. For every memory operation it compiles, WeaveC proves what it can, inserts a runtime check for the null and bounds obligations it cannot prove, and records everything else with a reason. It is built on Clang/LLVM, which does the parsing, code generation and platform support; WeaveC adds ownership inference, borrow and lifetime checking, check insertion and the safety ledger, in C++ libraries kept separate from the Clang integration.

> **Status:** WeaveC is early, v0.x software: flags, diagnostics and on-disk formats can change between minor versions. [RFC 0030](docs/rfcs/0030-prove-or-trap.md), *Prove or trap*, defines the model described here and is being implemented; the [roadmap](docs/roadmap.md) tracks progress.

## Quick look

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

int lookup(int i) {
  static const int table[4] = {10, 20, 30, 40};
  return i < 4 ? table[i] : -1;   // not proven for i < 0: checked at run time
}
```

```
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
weavec: example.c: 11 sites: 6 proven, 1 checkable (not enforced), 2 violations, 2 unresolved, 0 trusted; 2 errors, 1 warning
2 warnings and 2 errors generated.
Error while processing example.c.
```

Definite bugs are errors; a use-after-free on some paths only is a warning. The first warning is Clang's own. The last line summarises the file's *ledger*, which records an outcome for each safety facet (spatial, null, temporal) of every memory operation (*site*): proven, checked, a violation, or unresolved or trusted with a reason. The line counts each site by its worst facet. `table[i]` is counted as checkable: `weavec` only analyses, and `weavec-cc`, the compiler, turns it into a check that traps when `i` is negative.

Annotations (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_UNSAFE`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(q)`, `WEAVEC_STRING`, `WEAVEC_REQUIRE_SAFE`, `WEAVEC_ASSUME(e)` and the reference-counting forms) state contracts where inference needs help. They expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## What WeaveC guarantees

For a translation unit compiled by `weavec-cc` in an enforcing mode (the default `-fweavec-checks=trap`, or `verify`, with zero-initialisation on), every operation outside a `WEAVEC_UNSAFE` region satisfies:

- **(S)** if its spatial facet is proven or checked, it accesses only bytes inside the object its pointer was derived from, or the program traps first;
- **(N)** if its null facet is proven or checked, it does not dereference null, or the program traps first;
- **(T)** if its temporal facet is proven, the object is still alive, and a release releases a live allocation once;
- **(V)** a definite violation never reaches the object unguarded: it fails the build, or, if lowered with `-Wno-error`, traps.

These hold under five assumptions: **A1** callers outside the unit pass arguments that meet what it relies on; **A2** trusted callees (platform functions, the library table, declared contracts, code without WeaveC records) behave as their contracts say; **A3** other code leaves reachable pointers null or pointing to live objects, with owners unique; **A4** no other thread, signal handler or `longjmp` changes the memory outside sites marked for it; **A5** the allocator answers its usable-size query consistently, and memory from sources WeaveC does not zero is written before pointers are read from it. If a memory-safety violation happens anyway, then a check trapped first, or the ledger shows an unresolved or trusted facet (or an assumption at the unit's interface) that it rests on, or WeaveC has a bug, which `verify` mode monitors. Temporal bugs are not checked at run time. The full statement, what is caught and what is not, is in [the guarantees reference](docs/pages/reference/guarantees.md) and [RFC 0030, *Soundness*](docs/rfcs/0030-prove-or-trap.md#soundness).

## Modes

| `-fweavec-checks=` | Unproven null and bounds obligations | Zero-init | Guarantee |
| --- | --- | --- | --- |
| `trap` (default) | checked; a failed check traps | on | yes |
| `report` | checked; a failed check prints `weavec: runtime check failed: …` and continues (links `libweavec_rt.a`) | on | only with `WEAVEC_RT_ABORT=1` |
| `verify` | checked; proven facets are also checked where expressible, so a `weavec.proven` trap exposes a wrong proof | on | yes |
| `none` | nothing: the object is what Clang would produce | off | no |

- **Zero-initialisation.** In the checking modes, locals and the standard allocation calls are zero-initialised, so an uninitialised pointer is null and its checked dereference traps. `-fno-weavec-zero-init` turns it off.
- **Require levels.** `-fweavec-require=checked` makes every unresolved facet an `unresolved-operation` error; `-fweavec-require=proven` also makes every checked facet an `unchecked-operation` error. Trusted facets are allowed at every level. `WEAVEC_REQUIRE_SAFE` holds one function to `checked`.
- **Ledger.** `-fweavec-ledger=<path>` writes every site and facet with its outcome, reason, fix-it and a stable fingerprint, as JSON or, with `-fweavec-ledger-format=sarif`, SARIF 2.1.0. `-fweavec-summary` prints the one-line summary, which `weavec` always prints.

## Using it as the compiler

`weavec-cc` is Clang's driver with WeaveC inside. Point a build at it and it compiles as `clang` would, analyses and instruments each file as it compiles it, and checks the whole program when it links:

```sh
$ make CC=weavec-cc
weavec-cc -c node.c -o node.o          # node.o and node.o.weavec (its WeaveC record)
weavec-cc -c main.c -o main.o
weavec-cc node.o main.o -o prog        # reads the records, analyses the program, then links
```

A bug inside one file is reported when that file is compiled; a bug that needs two files is reported when they are linked, and an error stops the link. Link inputs without a WeaveC record (archives, shared libraries, objects from another compiler) are named in one `unanalyzed-input` warning.

The checks are ordinary C inserted before code generation, with no ABI change and no runtime library in the default mode. With `lookup` from the quick look in a program that passes it `atoi(argv[1])`:

```
$ weavec-cc -fweavec-summary lookup.c -o lookup
weavec: lookup.c: 7 sites: 5 proven, 2 checked, 0 unresolved, 0 trusted; 0 errors, 0 warnings
weavec: program lookup: 7 sites in 1 unit: 5 proven, 2 checked, 0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
$ ./lookup 2
30
$ ./lookup -1; echo "exit status $?"
exit status 133
```

The negative index stops the program with `SIGTRAP` (or `SIGILL`, depending on the target) at the access, instead of reading past the table. Other flags: `-fno-weavec` (plain Clang), `-fno-weavec-link` (skip the link-time step), `-fweavec-budget=<n>` (per-function analysis budget), `-Wno-error=weavec-<id>` (lower an error to a warning while migrating), `-Werror=weavec`. `weavec-cc --help-weavec` lists them all.

The tooling form analyses a compilation database without building: `weavec --whole-program -p build/` (all sources) or `weavec --whole-program a.c b.c -- -Iinclude`. Without `--whole-program`, `weavec file.c --` checks one file. Both take `--ledger`, `--ledger-format` and `--require`.

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
ctest --preset dev          # unit, lit and test/cases suites, in parallel
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
  Core/           Ownership lattice, lifetimes, borrows, moves, pointer kinds, the ledger,
                  the library table, check plans, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts: sites, kinds, the engine and the planner behind one seam
  Frontend/       Clang integration: deferred CodeGen, check emission, zero-init, ledger writers,
                  unit records, the whole-program step, the compiler driver
lib/              Implementations, mirroring include/ (lib/Core/LibrarySpec.txt is the library table)
runtime/          The small C runtime: report-mode reporting and out-of-line check helpers
tools/weavec/     The analysis tool (libTooling; --whole-program for a compilation database)
tools/weavec-cc/  The drop-in compiler driver (Clang's driver with WeaveC inside)
resources/        weavec.h, the C-facing annotation header (installed to lib/weavec/include)
unittests/        GoogleTest unit tests
test/             lit + FileCheck integration tests
  cases/          Executable C cases by feature, with markers, run by scripts/run-cases.py
  corpus/         Real projects pinned by SHA, expectations and triage, run by scripts/corpus-gate.py
scripts/          Test runners, the corpus gate, hygiene and release tooling
docs/             Architecture, RFCs (docs/rfcs/), roadmap, and the weavec.com site
cmake/            Build-system modules
```

The layering rule is strict: `Core` must not include anything from `clang/` or `llvm/`. `Analysis` is the only layer that knows about both worlds, and only the engine behind the `SafetyEngine` seam sees the dataflow internals. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) first. Security issues should be reported privately as described in [SECURITY.md](SECURITY.md).

## License

Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
