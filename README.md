# WeaveC

[Documentation](https://weavec.com) · [Getting started](https://weavec.com/getting-started/installation/) · [Diagnostic reference](https://weavec.com/reference/diagnostics/)

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

A mostly source-compatible C compiler that brings Rust-style memory safety to existing C code through inferred ownership and borrowing. For every memory operation it compiles, WeaveC proves what it can, inserts a runtime check for the null and bounds obligations it cannot prove but can state, guards what is left against a small runtime that knows each object's extent and whether it is still alive, and records everything else with a reason. It is built on Clang/LLVM, which does the parsing, code generation and platform support; WeaveC adds ownership inference, borrow and lifetime checking, check insertion and the safety ledger, in C++ libraries kept separate from the Clang integration.

> **Status:** WeaveC is early, v0.x software: flags, diagnostics and on-disk formats can change between minor versions. [RFC 0030](docs/rfcs/0030-prove-or-trap.md), *Prove or trap*, defines the model described here, and [RFC 0032](docs/rfcs/0032-runtime-enforcement.md), *Runtime enforcement*, adds the runtime and the `guarded` outcome; the [roadmap](docs/roadmap.md) tracks progress.

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
weavec: example.c: 11 sites: 6 proven, 1 checkable (not enforced), 2 guardable (not enforced), 2 violations, 0 unresolved, 0 trusted; 2 errors, 1 warning
2 warnings and 2 errors generated.
Error while processing example.c.
```

Definite bugs are errors; a use-after-free on some paths only is a warning. The first warning is Clang's own. The last line summarises the file's *ledger*, which records one of six outcomes for each safety facet (spatial, null, temporal) of every memory operation (*site*): proven, checked, guarded, a violation, or unresolved or trusted with a reason. The line counts each site by its worst facet. `table[i]` is counted as checkable, and the read in `maybe` and the `free` in `node_free` as guardable: `weavec` only analyses. `weavec-cc`, the compiler, turns the first into a check that traps when `i` is negative, and the other two into guards: the read traps if `n` was freed, and the `free` traps unless its argument is null or the start of a live heap block.

Annotations (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_UNSAFE`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(q)`, `WEAVEC_STRING`, `WEAVEC_REQUIRE_SAFE`, `WEAVEC_ASSUME(e)` and the reference-counting forms) state contracts where inference needs help. They expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## What WeaveC guarantees

For a translation unit compiled by `weavec-cc` in an enforcing mode (the default `-fweavec-checks=trap`, or `verify`, with zero-initialisation on), every operation outside a `WEAVEC_UNSAFE` region satisfies:

- **(S)** if its spatial facet is proven or checked, it accesses only bytes inside the object its pointer was derived from, or the program traps first;
- **(N)** if its null facet is proven or checked, it does not dereference null, or the program traps first;
- **(T)** if its temporal facet is proven, the object is still alive, and a release releases a live allocation once;
- **(G)** if a facet is guarded (the default; not with `-fno-weavec-runtime`) and the pointer points into an object the runtime tracks (a heap block from the image's allocator, a local whose address escapes, a global), the object is live and the access stays inside it, or the program traps first; a guarded release is of null or the start of a live heap block, or the program traps;
- **(V)** a definite violation never reaches the object unguarded: it fails the build, or, if lowered with `-Wno-error`, traps.

These hold under six assumptions: **A1** callers outside the unit pass arguments that meet what it relies on; **A2** trusted callees (platform functions, the library table, declared contracts, code without WeaveC records) behave as their contracts say; **A3** other code leaves reachable pointers null or pointing to live objects, with owners unique; **A4** no other thread, signal handler or `longjmp` changes the memory outside sites marked for it; **A5** the allocator answers its usable-size query consistently, and memory from sources WeaveC does not zero is written before pointers are read from it; **A6** a pointer a guard finds outside every tracked object points to a live object that the access stays inside. If a memory-safety violation happens anyway, then a check trapped first, or the ledger shows an unresolved, trusted or guarded facet (or an assumption at the unit's interface) that it rests on, or WeaveC has a bug, which `verify` mode monitors.

(G) is weaker than (S) and (T), which is why the ledger counts guarded facets apart from checked ones. A guard asks which object the pointer points into now, so arithmetic that carries a pointer out of one tracked object and into another live one passes at a plain dereference (a subscript `p[i]` is checked against the object that contains `p`). A freed heap block is caught only while it is in the quarantine (64 MiB by default); after its storage is reused the guard sees the new object. Memory the runtime does not track (string literals, `alloca` blocks, memory from other allocators or from code built without the runtime) passes. The full statement, what is caught and what is not, is in [the guarantees reference](docs/pages/reference/guarantees.md), [RFC 0030, *Soundness*](docs/rfcs/0030-prove-or-trap.md#soundness) and [RFC 0032, *Soundness*](docs/rfcs/0032-runtime-enforcement.md#soundness).

## Modes

| `-fweavec-checks=` | Unproven obligations | Zero-init | Runtime | Guarantee |
| --- | --- | --- | --- | --- |
| `trap` (default) | checked or guarded; a failed check or guard traps | on | linked | yes |
| `report` | checked or guarded; a failure prints `weavec: runtime check failed: …` and the program continues | on | linked | only with `WEAVEC_RT_ABORT=1` |
| `verify` | checked or guarded; proven facets are also checked or guarded where possible, so a `weavec.proven` trap exposes a wrong proof | on | linked | yes |
| `none` | nothing: the object is what Clang would produce | off | not linked | no |

- **The runtime.** Every enforcing link carries `libweavec_rt.a` (the object table: heap blocks with their exact sizes and a quarantine for freed ones, locals whose address escapes, globals) and `libweavec_alloc.a`, which defines `malloc`, `calloc`, `realloc` and `free` for the image. `-fno-weavec-runtime` builds without them: nothing is guarded, and the facets that would be stay `unresolved`. `weavec-cc` also builds without the runtime, and prints a note at the link, under a sanitizer that replaces the allocator, with `-ffreestanding` or `-nostdlib`, and on targets other than 64-bit Darwin and Linux; a program that defines `malloc` itself keeps its allocator, and its heap is untracked.

- **Zero-initialisation.** In the checking modes, locals and the standard allocation calls are zero-initialised, so an uninitialised pointer is null and its checked dereference traps. `-fno-weavec-zero-init` turns it off.
- **Require levels.** `-fweavec-require=guarded` makes every unresolved facet an `unresolved-operation` error; `-fweavec-require=checked` makes every guarded facet one too; `-fweavec-require=proven` also makes every checked facet an `unchecked-operation` error. Trusted facets are allowed at every level. `WEAVEC_REQUIRE_SAFE` holds one function to `checked`.
- **Possible findings.** A use-after-free or double free on some paths only is a warning in `weavec`. In an enforcing `weavec-cc` build with the runtime, such a finding on a guarded facet is not printed, because the guard traps if it happens; `-Wweavec-possible` prints it.
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

The checks and guards are ordinary C inserted before code generation, with no change to pointer representation or ABI; the guards call the runtime the link adds. With `lookup` from the quick look in a program that passes it `atoi(argv[1])`:

```
$ weavec-cc -fweavec-summary lookup.c -o lookup
weavec: lookup.c: 8 sites: 6 proven, 2 checked, 0 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings
weavec: program lookup: 8 sites in 1 unit: 6 proven, 2 checked, 0 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
$ ./lookup 2
30
$ ./lookup -1; echo "exit status $?"
exit status 133
```

The negative index stops the program with `SIGTRAP` (or `SIGILL`, depending on the target) at the access, instead of reading past the table. Where the code states no bound, a guard asks the runtime. Here `main` allocates four `int`s for `v.data` and calls `get` with `atoi(argv[1])`:

```c
struct vec { int *data; size_t len; };
int get(struct vec *v, size_t i) { return v->data[i]; }
```

```
$ weavec-cc -fweavec-summary vec.c -o vec
weavec: vec.c: 11 sites: 8 proven, 2 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings
weavec: program vec: 11 sites in 1 unit: 8 proven, 2 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
$ ./vec 1000; echo "exit status $?"
exit status 133
```

The extent of `v->data` is unknown to the analysis (`unknown-extent`), so the access is `guarded`: the runtime finds the 16-byte block and the guard traps before the read.

Other flags: `-fno-weavec` (plain Clang), `-fno-weavec-link` (skip the link-time step), `-fweavec-budget=<n>` (per-function analysis budget), `-Wno-error=weavec-<id>` (lower an error to a warning while migrating), `-Werror=weavec`. `weavec-cc --help-weavec` lists them all.

The tooling form analyses a compilation database without building: `weavec --whole-program -p build/` (all sources) or `weavec --whole-program a.c b.c -- -Iinclude`. Without `--whole-program`, `weavec file.c --` checks one file. Both take `--ledger`, `--ledger-format`, `--require` and `--no-runtime` (model a build with `-fno-weavec-runtime`).

### What the runtime costs

Guards run on every execution of the operations they cover, and the allocator replaces the system's. Measured on the benchmarks of the corpus (user CPU time, and peak memory in the default mode, relative to the same program built by the reference Clang; [RFC 0032](docs/rfcs/0032-runtime-enforcement.md#implementation-amendments), amendment 3):

| Benchmark | Default (runtime) | `-fno-weavec-runtime` | Peak memory (default) |
| --- | --- | --- | --- |
| cJSON parse/print | 1.66× | 1.14× | 0.59× |
| zlib minigzip | 1.85× | 1.00× | 1.09× |
| Lua bench | 5.94× | 1.11× | 1.61× |

Interpreters and tight loops over pointers pay the most: Lua's dispatch loop executes roughly one guard for every two instructions of the unguarded program. Code that cannot pay this builds with `-fno-weavec-runtime`; its facets that would be guarded are then `unresolved` and not enforced, and a use of a freed object is not caught at run time.

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
runtime/          The C runtime: the allocator and object table, failure reports, out-of-line check helpers
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

The layering rule is strict: `Core` must not include anything from `clang/` or `llvm/`. `Analysis` is the only layer that knows about both worlds, and only the object engine behind the `SafetyEngine` seam (`lib/Analysis/Engine*.cpp`) sees its own internals. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) first. Security issues should be reported privately as described in [SECURITY.md](SECURITY.md).

## License

Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
