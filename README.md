<div align="center">

# WeaveC

**Memory safety for existing C code, through inferred ownership and borrowing.**

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/weavefoundry/weavec?include_prereleases&sort=semver)](https://github.com/weavefoundry/weavec/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

[Documentation](https://weavec.com) · [Getting started](https://weavec.com/getting-started/installation/) · [Diagnostic reference](https://weavec.com/reference/diagnostics/) · [Releases](https://github.com/weavefoundry/weavec/releases)

</div>

WeaveC is a mostly source-compatible C compiler that brings Rust-style memory safety to the C code you already have. It infers which pointers own memory and which borrow it, and for every memory operation it compiles, it settles each safety question one of four ways:

1. **Prove** it safe at compile time.
2. **Check** it at run time, when the obligation (a null test or a bound) can be stated.
3. **Guard** it at run time against a small runtime that knows each object's extent and whether it is still alive.
4. **Record** it in the safety *ledger* as unresolved or trusted, with a reason.

Definite bugs fail the build. Everything else is accounted for, operation by operation, in the ledger.

WeaveC is built on Clang and LLVM, which provide parsing, code generation and platform support. WeaveC adds ownership inference, borrow and lifetime checking, check insertion and the ledger, in C++ libraries kept separate from the Clang integration.

> [!NOTE]
> WeaveC is early, v0.x software: flags, diagnostics and on-disk formats can change between minor versions. The model is defined by [RFC 0030, *Prove or trap*](docs/rfcs/0030-prove-or-trap.md), and [RFC 0032, *Runtime enforcement*](docs/rfcs/0032-runtime-enforcement.md) adds the runtime and the `guarded` outcome; [RFC 0033, *Drop-in by default*](docs/rfcs/0033-drop-in-by-default.md) makes the default build accept correct code it was never tuned on. The [roadmap](docs/roadmap.md) tracks progress.

## Highlights

- **A drop-in compiler.** `weavec-cc` is Clang's driver with WeaveC inside: point a build at it with `CC=weavec-cc` and it compiles as `clang` would. A correct program built with the default flags compiles, links and runs as it does with Clang, apart from run time; a false error or trap is a WeaveC bug.
- **Inference first.** Ownership contracts are inferred from function bodies. Annotations are needed only where inference needs help, and they expand to nothing on other compilers.
- **No ABI changes.** Checks and guards are ordinary C inserted before code generation; pointer representation is unchanged.
- **Whole-program analysis.** Bugs within a file are reported when it is compiled; bugs that span files are reported by `weavec --whole-program` or a link with `-fweavec-link=analyze`, and guarded at run time otherwise.
- **An auditable ledger.** Every safety facet of every memory operation is recorded with its outcome, reason and a stable fingerprint, as JSON or SARIF 2.1.0.
- **Adjustable strictness.** Start by recording what cannot be proven, then require guarded, checked or proven operations as the code matures.

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
weavec: example.c: 11 sites: 6 proven, 1 checkable (not enforced), 2 guardable (not enforced), 2 violations, 0 unresolved, 0 trusted; 2 errors, 1 warning
2 warnings and 2 errors generated.
Error while processing example.c.
```

Definite bugs are errors; a use-after-free on only some paths is a warning. (The first warning is Clang's own.)

The last line summarises the file's ledger, counting each memory operation (*site*) by its worst outcome. `weavec` only analyses, so it reports what the compiler *would* enforce as "checkable" and "guardable". Built with `weavec-cc`:

- `table[i]` gets a **check** that traps when `i` is negative;
- the read of `n->v` in `maybe` gets a **guard** that traps if `n` was freed;
- the `free` in `node_free` gets a guard that traps unless its argument is null or the start of a live heap block.

## How it works

WeaveC considers up to three safety *facets* of every memory operation: **spatial** (the bytes it touches lie inside its object), **null** (it does not dereference null) and **temporal** (the object is still alive, and a release releases a live allocation once). Each facet receives exactly one outcome in the ledger:

| Outcome | Meaning | Enforcement in `weavec-cc` |
| --- | --- | --- |
| `proven` | The facet holds on every execution. | None needed. |
| `checked` | Not proven, but the code states what to check against. | An inserted check traps before the operation if it would fail. |
| `guarded` | Not proven, and not checkable from what the code states. | A guard looks the pointer up in the runtime's object table and traps if the access leaves its object or the object is dead. |
| `violation` | The facet fails on every execution that reaches it. | A build error. |
| `unresolved` | None of the above; recorded with a reason such as `raw-cast` or `unknown-callee`. | Not enforced. |
| `trusted` | Holds if a named trust assumption holds, such as a `WEAVEC_UNSAFE` region or a platform C function. | Not enforced. |

### Annotations

Where inference needs help, typically at public interfaces, annotations from `weavec.h` state the contract: `WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_UNSAFE`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(q)`, `WEAVEC_STRING`, `WEAVEC_REQUIRE_SAFE`, `WEAVEC_ASSUME(e)` and the reference-counting forms. They expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## Using it as the compiler

Point a build at `weavec-cc`. It analyses and instruments each file as it compiles it, and checks the files' records against each other when it links:

```sh
$ make CC=weavec-cc
weavec-cc -c node.c -o node.o          # node.o and node.o.weavec (its WeaveC record)
weavec-cc -c main.c -o main.o
weavec-cc node.o main.o -o prog        # checks the records against each other, then links
```

A bug inside one file is reported when that file is compiled. By default the link step only reads the records: it checks declarations against definitions and the requirements of each exported function at the calls other files make, and analyses no file again. A bug that needs two files, such as a use after free whose `free` is in another file, is caught by its guard at run time; to report it at build time, link with `-fweavec-link=analyze` (which analyses the units again, within a time budget) or run `weavec --whole-program`. Link inputs without a WeaveC record (archives, shared libraries, objects from another compiler) are named in one `unanalyzed-input` warning.

### Checks

Take `lookup` from the quick look, in a program that passes it `atoi(argv[1])`:

```console
$ weavec-cc -fweavec-summary lookup.c -o lookup
weavec: lookup.c: 8 sites: 6 proven, 2 checked, 0 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings
weavec: program lookup: 8 sites in 1 unit: 6 proven, 2 checked, 0 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
$ ./lookup 2
30
$ ./lookup -1; echo "exit status $?"
exit status 133
```

The negative index stops the program with `SIGTRAP` (or `SIGILL`, depending on the target) at the access, instead of reading past the table.

### Guards

Where the code states no bound, a guard asks the runtime. Here `main` allocates four `int`s for `v.data` and calls `get` with `atoi(argv[1])`:

```c
struct vec { int *data; size_t len; };
int get(struct vec *v, size_t i) { return v->data[i]; }
```

```console
$ weavec-cc -fweavec-summary vec.c -o vec
weavec: vec.c: 11 sites: 8 proven, 2 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings
weavec: program vec: 11 sites in 1 unit: 8 proven, 2 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
$ ./vec 1000; echo "exit status $?"
exit status 133
```

The extent of `v->data` is unknown to the analysis (`unknown-extent`), so the access is `guarded`: the runtime finds the 16-byte block and the guard traps before the read.

### Analysis without building

The `weavec` tool analyses without producing binaries. `weavec file.c --` checks one file; `weavec --whole-program -p build/` analyses every source in a compilation database, and `weavec --whole-program a.c b.c -- -Iinclude` analyses the given sources as one program. Both forms accept `--ledger`, `--ledger-format`, `--require` and `--no-runtime` (which models a build with `-fno-weavec-runtime`).

## Modes and options

`-fweavec-checks=` selects what unproven obligations become:

| Mode | Unproven obligations | Zero-init | Runtime | Guarantee |
| --- | --- | --- | --- | --- |
| `trap` (default) | Checked or guarded; a failed check or guard traps. | On | Linked | Yes |
| `report` | Checked or guarded; a failure prints `weavec: runtime check failed: …` and the program continues. | On | Linked | Only with `WEAVEC_RT_ABORT=1` |
| `verify` | As `trap`, and proven facets are also checked or guarded where possible, so a `weavec.proven` trap exposes a wrong proof. | On | Linked | Yes |
| `none` | Nothing: the object file is what Clang would produce. | Off | Not linked | No |

- **The runtime.** Every enforcing link carries `libweavec_rt.a`, the object table (heap blocks with their exact sizes and a quarantine for freed ones, locals whose address escapes, globals), and `libweavec_alloc.a`, which defines `malloc`, `calloc`, `realloc` and `free` for the image. On Darwin one runtime serves the whole process, and its allocator also serves the C library's own allocations (`strdup`, `getline`, `asprintf`). `-fno-weavec-runtime` builds without them: nothing is guarded, and the facets that would be stay `unresolved`. `weavec-cc` also builds without the runtime, and prints a note at the link, under a sanitizer that replaces the allocator, with `-ffreestanding` or `-nostdlib`, and on targets other than 64-bit Darwin and Linux. A program that defines `malloc` itself keeps its allocator, and its heap is untracked.
- **Zero-initialisation.** In the checking modes, locals and the standard allocation calls are zero-initialised, so an uninitialised pointer is null and its checked dereference traps. `-fno-weavec-zero-init` turns this off.
- **Require levels.** `-fweavec-require=guarded` makes every unresolved facet an `unresolved-operation` error; `-fweavec-require=checked` makes every guarded facet one too; `-fweavec-require=proven` also makes every checked facet an `unchecked-operation` error. Trusted facets are allowed at every level. `WEAVEC_REQUIRE_SAFE` holds a single function to `checked`.
- **Possible findings.** A use-after-free or double free on only some paths is a warning in `weavec`. In an enforcing `weavec-cc` build with the runtime, such a finding on a guarded facet is not printed, because the guard traps if it happens; `-Wweavec-possible` prints it anyway.
- **Ledger.** `-fweavec-ledger=<path>` writes every site and facet with its outcome, reason, fix-it and stable fingerprint, as JSON or, with `-fweavec-ledger-format=sarif`, SARIF 2.1.0. `-fweavec-summary` prints the one-line summary, which `weavec` always prints.

Other useful flags:

| Flag | Effect |
| --- | --- |
| `-fno-weavec` | Compile as plain Clang. |
| `-fweavec-link=analyze` | Analyse the units again at link with the whole program in view, within `-fweavec-link-budget=<seconds>` (default 120). |
| `-fweavec-link=none` | Skip the link step (the default, `records`, only reads the units' records). |
| `-fweavec-budget=<n>`, `-fweavec-unit-budget=<n>` | Set the per-function and per-unit analysis budgets. |
| `-Wno-error=weavec-<id>` | Lower an error to a warning while migrating; the site is guarded or checked instead. |
| `-Wweavec-leak` | Print leak warnings, which `weavec-cc` leaves off by default. |
| `-Werror=weavec` | Promote WeaveC warnings to errors. |

`weavec-cc --help-weavec` lists every option; the [command-line reference](docs/pages/reference/cli.md) documents them in full.

## What WeaveC guarantees

For a translation unit compiled by `weavec-cc` in an enforcing mode (the default `-fweavec-checks=trap`, or `verify`, with zero-initialisation on), every operation outside a `WEAVEC_UNSAFE` region satisfies:

- **(S) Spatial.** If its spatial facet is proven or checked, it accesses only bytes inside the object its pointer was derived from, or the program traps first.
- **(N) Null.** If its null facet is proven or checked, it does not dereference null, or the program traps first.
- **(T) Temporal.** If its temporal facet is proven, the object is still alive, and a release releases a live allocation once.
- **(G) Guards.** If a facet is guarded (the default; not with `-fno-weavec-runtime`) and the bytes it accesses start inside an object the runtime tracks (a heap block from the runtime's allocator, a local whose address escapes, a global), the object is live and the access stays inside it, or the program traps first. A guarded release is of null or the start of a live heap block, or the program traps.
- **(V) Violations.** A definite violation never reaches the object unguarded: it fails the build, or, if lowered with `-Wno-error`, the site is guarded or checked, and traps unconditionally only where it can be neither.

These hold under six assumptions:

| | Assumption |
| --- | --- |
| **A1** | Callers outside the unit pass arguments that meet what it relies on. |
| **A2** | Trusted callees (platform functions, the library table, declared contracts, code without WeaveC records) behave as their contracts say. |
| **A3** | Other code leaves reachable pointers null or pointing to live objects, with owners unique. |
| **A4** | No other thread, signal handler or `longjmp` changes the memory outside sites marked for it. |
| **A5** | The allocator answers its usable-size query consistently, and memory from sources WeaveC does not zero is written before pointers are read from it. |
| **A6** | A pointer a guard finds outside every tracked object points to a live object that the access stays inside. |

If a memory-safety violation happens anyway, then a check trapped first, or the ledger shows an unresolved, trusted or guarded facet (or an assumption at the unit's interface) that it rests on, or WeaveC has a bug, which `verify` mode monitors.

### Limits of guards

(G) is weaker than (S) and (T), which is why the ledger counts guarded facets separately from checked ones:

- **Provenance is the address accessed.** A guard asks which object the bytes an access touches lie in *now*, at a subscript `p[i]` as at a dereference, so arithmetic that carries a pointer out of one tracked object and into another live one passes. (This is what lets a base formed outside a buffer, such as `base = src - start`, index back into it.) Two cases still trap: an index or field offset that reaches forwards from inside a live local or global into another one, since those lie next to each other with no gap, and an index or offset from a pointer into a live object that lands in memory nothing tracks. Between heap blocks there is always a byte that belongs to none, so a walk off a heap block traps at its first step.
- **Freed memory is recycled.** A freed heap block is caught only while it is in the quarantine (64 MiB by default); once its storage is reused, the guard sees the new object.
- **Untracked memory passes.** String literals, `alloca` blocks, and memory from other allocators or from code built without the runtime are not tracked; a guard passes on them unless an index carried the pointer there from a live tracked object.

The full statement, including what is and is not caught, is in [the guarantees reference](docs/pages/reference/guarantees.md), [RFC 0030, *Soundness*](docs/rfcs/0030-prove-or-trap.md#soundness), [RFC 0032, *Soundness*](docs/rfcs/0032-runtime-enforcement.md#soundness) and [RFC 0033, *Soundness*](docs/rfcs/0033-drop-in-by-default.md#soundness).

## Performance

Guards run on every execution of the operations they cover, and the runtime's allocator replaces the system's. Measured on the corpus benchmarks as user CPU time and peak memory, relative to the same program built by the reference Clang ([RFC 0032](docs/rfcs/0032-runtime-enforcement.md#implementation-amendments), amendment 3):

| Benchmark | Default (runtime) | `-fno-weavec-runtime` | Peak memory (default) |
| --- | ---: | ---: | ---: |
| cJSON parse/print | 1.66× | 1.14× | 0.59× |
| zlib minigzip | 1.85× | 1.00× | 1.09× |
| Lua bench | 5.94× | 1.11× | 1.61× |

Interpreters and tight loops over pointers pay the most: Lua's dispatch loop executes roughly one guard for every two instructions of the unguarded program. Code that cannot afford this can build with `-fno-weavec-runtime`; facets that would have been guarded are then `unresolved` and not enforced, and a use of a freed object is not caught at run time.

## Building

**Requirements:** CMake ≥ 3.24, Ninja, a C++20 compiler, and an LLVM/Clang development install (23.x recommended; ≥ 20 supported). The tests also need `lit` and LLVM's `FileCheck`.

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
ctest --preset dev          # unit, lit and test/cases suites, in parallel
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
  Core/           Ownership lattice, lifetimes, borrows, moves, pointer kinds, the ledger,
                  the library table, check plans, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts: sites, kinds, the engine and the planner behind one seam
  Frontend/       Clang integration: deferred CodeGen, check emission, zero-init, ledger writers,
                  unit records, the link step, the compiler driver
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

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md) first. Report security issues privately, as described in [SECURITY.md](SECURITY.md).

## License

WeaveC is licensed under the Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
