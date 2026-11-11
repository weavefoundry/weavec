<div align="center">

# WeaveC

**Memory safety for existing C and C++ code: every access checked at run time, freed memory never handed back while a pointer to it remains.**

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/weavefoundry/weavec?include_prereleases&sort=semver)](https://github.com/weavefoundry/weavec/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

[Documentation](https://weavec.com) · [Getting started](https://weavec.com/getting-started/installation/) · [Safety guarantees](https://weavec.com/reference/guarantees/) · [Releases](https://github.com/weavefoundry/weavec/releases)

</div>

WeaveC is a drop-in C and C++ compiler, `weavec-cc` and `weavec-c++`, that makes the code you already have stop at its first bad memory access. Before a load, a store or a C library call touches memory, a few inline instructions ask a small runtime linked into the program whether those bytes belong to a live object. The first access that touches a byte that does not stops the program with a report that names the access.

- **Every access is guarded.** An access past a heap block, a local or a global, a use of a local after its scope, an index past an array's declared bound and a bad `free` stop the program. A guard is removed only where a simple local rule proves it redundant.
- **Freed memory is reused only after a sweep.** A released heap block waits until a sweep of the program's stacks, registers, writable data and live heap finds no pointer to it. A use after free or a double free traps, however much the program allocated since.
- **Fortified sizes are kept.** A fortified library call never writes more than the size it was given: with Darwin's headers, a `strcpy` that would run from an 8-byte field into the next one traps.
- **C++ too.** `new` and `delete` are checked against each other, a `std::vector`'s storage past its elements is not addressable, the C++ library's own bounds checks report through WeaveC, and an exception leaves the live frames' redzones in place.
- **Safe C, when you want it.** A C function marked `WEAVEC_SAFE` (or every function, with `-fweavec-safe`) is also checked when it compiles: a few local rules reject what could forge a pointer or let a borrowed one escape, each error saying what to write instead, and its accesses are checked against the bounds its pointers' declarations give.

WeaveC is built on Clang and LLVM, which provide parsing, code generation and platform support. WeaveC adds LLVM passes that guard the code, and a runtime in C.

> [!NOTE]
> WeaveC is early, v0.x software: flags, report formats and the ledger can change between minor versions. The design is [RFC 0041, *Safe C*](docs/rfcs/0041-safe-c.md), which adds safe functions checked when they compile and against their pointers' bounds when they run, over [RFC 0040, *Reports you can act on*](docs/rfcs/0040-actionable-reports.md), which gives every report its stacks and names its object, [RFC 0039, *C++*](docs/rfcs/0039-cxx.md), which guards C++ as C, [RFC 0038, *Drop-in everywhere*](docs/rfcs/0038-drop-in-everywhere.md), which makes one shared runtime serve every image on every platform and ships self-contained toolchains, the sweep and colors of [RFC 0037](docs/rfcs/0037-complete-sweep-and-colors.md), the reuse after a sweep of [RFC 0036](docs/rfcs/0036-deterministic-heap-reuse.md) and the guards of [RFC 0035](docs/rfcs/0035-guard-by-default.md). The [roadmap](docs/roadmap.md) tracks progress.

## Highlights

- **A drop-in compiler.** `weavec-cc` is Clang's driver with WeaveC inside: point a build at it with `CC=weavec-cc CXX=weavec-c++` and it compiles as `clang` and `clang++` would. A correct program built with the default flags compiles, links and runs as it does with Clang, apart from run time; a false trap is a WeaveC bug.
- **Nothing to annotate.** Existing code builds unchanged and every access is guarded. `WEAVEC_UNSAFE` from `weavec.h` leaves a function or a block unguarded, for code that reads past an object on purpose; the other annotations are for [safe functions](docs/pages/guides/safe-c.md), which code opts into.
- **Reports you can act on.** A failed guard prints the kind of failure, the source location and the address, the stack of the access, and where the address lies: for the heap, the block and the stacks that allocated and released it; for a local or a global, its name. Release builds keep the line tables this needs, and `WEAVEC_RT_REPORT_FORMAT=json` writes each report as one line of JSON.
- **No ABI changes.** Pointers keep their representation and calling conventions are unchanged; objects from other compilers link in.
- **Compatible with AddressSanitizer's contract.** Code compiled by `weavec-cc` sees `__SANITIZE_ADDRESS__` and `__has_feature(address_sanitizer)`, and the runtime implements ASan's poisoning interface, so code written to be ASan-clean is WeaveC-clean, and a pool that poisons its free space for ASan is checked by WeaveC too.
- **An auditable ledger.** `-fweavec-ledger` lists every access the compiler saw, whether it kept its guard, and why not.

## Quick look

A read past a heap block, built with `weavec-cc` and run:

```c
struct vec { int *data; size_t len; };
int get(struct vec *v, size_t i) { return v->data[i]; }   // v->data holds 4 ints
```

```console
$ weavec-cc vec.c -o vec
$ ./vec 4
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xc080000010
    #0 0x1041e0853 in get vec.c:9
    #1 0x1041e0aeb in main vec.c:15
    #2 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: 0xc080000010 is 0 bytes after the 16-byte heap object at 0xc080000000, allocated by thread T0:
    #0 0x1041e095b in main vec.c:12
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: summary: heap-buffer-overflow at vec.c:9:43 in get (thread T0)
```

The program stops with `SIGTRAP` (or `SIGILL` on x86-64) at the access, instead of reading past the block.

A stale pointer stays caught however much the program allocates after the `free`:

```c
struct session *s = malloc(sizeof *s);
s->on_close = bye;
free(s);
for (int i = 0; i < 4000000; i++)
  free(malloc(sizeof *s));   // the allocator recycles millions of blocks of the same size
s->on_close();               // but never the one s still points to
```

```console
$ weavec-cc stale.c -o stale
$ ./stale
weavec: heap-use-after-free at stale.c:18:3: read of 8 bytes at 0xbe00000000
    #0 0x10412c737 in main stale.c:18
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: 0xbe00000000 is 0 bytes inside the 8-byte heap block at 0xbe00000000, released by thread T0:
    #0 0x10412c64f in main stale.c:15
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: the block was allocated by thread T0:
    #0 0x10412c573 in main stale.c:11
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: summary: heap-use-after-free at stale.c:18:3 in main (thread T0)
```

The same guards stop an overflow of a local or a global (`stack-buffer-overflow`, `global-buffer-overflow`), a use of a local after its scope (`stack-use-after-scope`), a fortified call that overruns a field (`intra-object-overflow`), an access to memory the program poisoned through ASan's interface (`use-after-poison`), an index past an array's declared bound (`index-out-of-bounds`), and a double or invalid `free` or `delete` (`double-free`, `invalid-free`, `alloc-release-mismatch`), each with its location and stacks.

## How it works

The runtime replaces the program's allocator and keeps one *shadow* byte for every 16 bytes of memory, saying which of them belong to a live object. Every heap object sits in a slot of an arena with at least one byte after it that belongs to no object. A released block waits in a quarantine until a *sweep*, which stops the program's threads and scans their stacks, registers, writable data and live heap conservatively, finds no pointer to it; only then is it reused. Sweeps are paced so that they take at most about a ninth of the run time; between them the quarantine grows, up to a ceiling. `weavec-cc` lays out the locals a guard can reach in one frame with *redzones* between them (locals whose lifetimes never overlap share space, as Clang's own frames do), gives the file's globals redzones, and zero-initializes locals.

Four LLVM passes then guard the code. The first puts a guard before every load, store, atomic operation and memory intrinsic, and before every memory argument of a call in the [library table](lib/Core/LibrarySpec.txt), before the optimizer runs, so the optimizer cannot exploit a bug before the guard sees it. The others remove the guards a local rule proves redundant (an access inside a local or a global at a known offset, an access a dominating guard on the same pointer already covered with nothing between that may free, nearby accesses merged into one guard, a loop whose whole range is checked once before it runs) and expand the rest into a three-instruction check of the shadow that calls the runtime only when a byte is not zero. Array indexes are also checked against their declared bounds, and fortified library calls against the object size they carry. `-fweavec-checks=verify` keeps every removed guard as a monitor, so a wrong removal is caught in testing.

## Using it as the compiler

Point a build at `weavec-cc` and `weavec-c++`:

```sh
make CC=weavec-cc CXX=weavec-c++
cmake -S . -B build -DCMAKE_C_COMPILER=weavec-cc -DCMAKE_CXX_COMPILER=weavec-c++
```

Every link, of a program or a shared library, adds the runtime, one shared library (`libweavec_rt.so.<abi>`, `libweavec_rt.dylib`) that every image `weavec-cc` links depends on, so one runtime serves the whole process whatever its libraries' version scripts or link order, and its allocator also serves the C library's own allocations (`strdup`, `getline`, `asprintf`). A program loads it from the toolchain, or from where `-fweavec-rpath` says; a `-static` Linux program carries it. `weavec-cc` builds without the runtime, and without guards, under a sanitizer that replaces the allocator, with `-ffreestanding` or `-nostdlib`, and on targets other than 64-bit Darwin and Linux, and prints a note at the link. A program that defines `malloc` itself keeps its allocator, and its heap is untracked. On optimizing glibc builds `weavec-cc` adds `-D_FORTIFY_SOURCE=3` unless the command line chooses a level, and C++ compiles get the C++ library's bounds checks (libc++'s fast hardening mode, libstdc++'s assertions).

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
| `-fno-weavec-zero-init` | Do not zero-initialize locals. |
| `-fweavec-ledger=<dir>/`, `-fweavec-summary` | Write the enforcement ledger per unit; print `weavec: file.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded`. |
| `-fno-weavec-asan-compat` | Do not compile as AddressSanitizer would for the source. |
| `WEAVEC_RT_STATS=1` | Print the runtime's counters (allocations, releases, sweeps and their pauses, recorded stacks, memory the runtime does not track) when the program exits. |
| `WEAVEC_RT_REPORT_LOG=<path>` | Write reports to a file instead of stderr. |
| `WEAVEC_RT_REPORT_FORMAT=json` | Write each report as one line of JSON. |
| `WEAVEC_RT_HEAP_STACKS=<n>` | Keep `n` frames of each allocation and release stack (8 by default, at most 32); `0` records none. |
| `WEAVEC_RT_SYMBOLIZE=0` | Name a report's frames by module and offset only; `weavec-cc --weavec-symbolize <log>` names them later. |
| `WEAVEC_RT_QUARANTINE=<bytes>` | Change how much released memory waits before a sweep (16 MiB by default, and at least a quarter of the live heap). |
| `WEAVEC_RT_SWEEP_SIGNAL=<n>` | On Linux, the signal that stops threads for a sweep (`SIGPWR` by default). |

`weavec-cc --help-weavec` lists every option; the [command-line reference](docs/pages/reference/cli.md) documents them in full.

## What WeaveC guarantees

For a program whose every unit is compiled by `weavec-cc` (or `weavec-c++`) in an enforcing mode (`trap` or `verify`), with the runtime linked, every memory access outside a `WEAVEC_UNSAFE` region satisfies:

- **(A) Accesses.** A load, store, atomic operation or memory intrinsic executes only if every byte it touches is *addressable* when it runs, and so does each memory argument of a call to a function in the library table; otherwise the program traps first.
- **(B) Bounds.** An index into an array whose bound its type declares (other than a trailing array of a struct) is within that bound, or the program traps.
- **(R) Releases.** A release through the runtime's allocator releases null or the start of a live heap object allocated by the matching form (`malloc` for `free`, `new` for `delete`, `new[]` for `delete[]`), and a sized `delete` names the object's size, or the program stops.
- **(K) Containers.** A byte of a `std::vector`'s or `std::deque`'s storage outside its elements is not addressable while the C++ library's code keeps its annotations.
- **(T) Temporal.** A byte of a released heap block is not addressable until a sweep has found no pointer to the block. An access through a pointer the program held at the release traps, and so does a second release.
- **(F) Fortified sizes.** A fortified library call writes no more than the object size it was given, or the program traps.
- **(P) Poisoning.** A byte the program poisoned through ASan's interface is not addressable until it unpoisons it.
- **(U) Use after return.** A local of a frame that returned, or that an exception unwound, is not addressable until another frame reuses its bytes.

A byte is not addressable when it lies after a heap object in its slot, in a released block, in a redzone of a tracked local or global, in a local outside its scope or of a function that returned, or in the lowest 64 KiB. Each tracked object also carries one of 111 colors, and an access computed from a pointer traps when it lands in an object of another color, so an index that jumps over a redzone into a live neighbor stops; for objects that are not neighbors a collision is 1 in 111. So, deliberately, these pass: a jump into an object of the same color, from a pointer into the middle of a heap block, or through a pointer one function moved into another object and another function used; a use after return through a frame a later call reused; a pointer jump from one field of a struct into the next by arithmetic (a string or formatting call into a field is checked against it); an access through a pointer the program hid from the sweep (stored as a transformed integer, XOR-linked, held only by the kernel); accesses to memory the runtime does not track (other allocators that do not poison for ASan, `mmap`, string literals); and code that `weavec-cc` did not compile. The guarantee assumes that untracked memory is accessed correctly, that library functions access only what the library table says, that there are no data races between a guard and its access, and that the program does not write the runtime's shadow.

The full statement is in [the guarantees reference](docs/pages/reference/guarantees.md) and [RFC 0037, *Soundness*](docs/rfcs/0037-complete-sweep-and-colors.md#soundness).

## Status

RFC 0035 put a guard on every access. Measured at v0.16.0, before RFC 0036: ten C projects WeaveC was never tuned on (git, tcpdump, libtiff, BearSSL, YARA, gawk, libarchive, Tcl, json-c, MicroPython) built with their own build systems and `CC=weavec-cc` and passed their test suites; CPython 3.13.7, SQLite 3.50.4 and GNU coreutils 9.5 built with configure results identical to the reference compiler's; and the measured workloads ran at or below AddressSanitizer's cost (SQLite's `speedtest1` 1.55× the reference against ASan's 1.81×, Lua 2.10× against 2.73×), with peak memory 1.2–1.5× (ASan 2.4–6×).

RFC 0036 makes use after free and double free deterministic (reuse only after a sweep), keeps fortified sizes, honors ASan's contract and lets large frames share space between locals. Measured on darwin-arm64 (its [measured gates](docs/rfcs/0036-deterministic-heap-reuse.md#measured-gates)): CPython 3.13.7, SQLite 3.50.4 and coreutils 9.5 configure as with the reference compiler and pass their suites as it does, but for a deliberate null read in CPython's `test_faulthandler` and four coreutils tests that reach gnulib's word-at-a-time `rawmemchr` over-read, which ASan stops too; every corpus project builds and passes its tests; the corpus benchmarks run at a geometric mean of 1.6× the reference compiler (1.05× v0.16.0; QuickJS 2.4×, because it sends every object to `malloc` when it sees ASan's macros), with peak memory within 1.5× v0.16.0's; and a sweep over 256 MiB of live heap pauses the program for 33 ms. Interpreters and allocation-heavy programs pay the most: measure on your own workload.

RFC 0037 makes the sweep read every pointer v0.17.0's missed (vector registers, coroutine stacks, thread-specific data, memory made read-only, file pages written back), closes holes in the guards (a loop range that wrapped, tagged pointers, large atomics) and in the library calls (58 of 84 probes stop, from 24), gives the allocator thread caches and marks in parallel, and catches jumps into a neighbor and uses after return. Measured on darwin-arm64 (its [measured gates](docs/rfcs/0037-complete-sweep-and-colors.md#measured-gates)): the corpus benchmarks run at 1.72× the reference compiler (1.05× v0.17.0); a sweep over 1 GiB of live heap pauses for at most 71 ms (289 before); allocation at 8 threads is 12 times v0.17.0's; Redis at ~1 GiB serves 0.86–0.88 of the reference's requests with its longest request at about 60 ms (150 before). Builds are up to 1.2× v0.17.0's.

RFC 0038 makes the runtime one shared library per process, so shared libraries and plugins built with `weavec-cc` are checked on Linux as on macOS; checks search calls (`strchr`, `memchr`, `strcspn` and the like) over the bytes they actually read; and ships self-contained toolchains that need no LLVM installation. Measured on darwin-arm64 and on linux-aarch64 (its [measured gates](docs/rfcs/0038-drop-in-everywhere.md#measured-gates)): SQLite 3.50.4, CPython 3.13.7, PostgreSQL 17.6, git 2.51.0 and coreutils 9.5 build with `CC=weavec-cc` and fail only the tests that fail with the reference compiler, but for two coreutils tests on Linux that compare stderr under `ulimit -v`, where WeaveC runs unchecked and says so on stderr; five projects held back until the end (jq, tmux, nginx, libarchive, libssh2) build and pass their suites with no report; every corpus project passes in both modes on both platforms, zstd included; and the corpus benchmarks run at 0.99× v0.18.0's CPU time on darwin-arm64 (1.70× the reference compiler). linux-x86_64 is not yet measured.

## Installing

Each [release](https://github.com/weavefoundry/weavec/releases) publishes a self-contained toolchain for macOS on Apple silicon and Linux on x86-64 and AArch64, `weavec-X.Y.Z-<os>-<arch>.tar.xz`: unpack it anywhere and put its `bin` on `PATH`. It needs no LLVM installation, only the platform's linker and C library headers (Xcode's Command Line Tools, or `build-essential`). Or `brew install weavefoundry/tap/weavec`, or the container image `ghcr.io/weavefoundry/weavec:X.Y.Z`. See [installation](docs/pages/getting-started/installation.mdx).

```sh
tar -xJf weavec-X.Y.Z-linux-x86_64.tar.xz -C "$HOME/.local/opt"
export PATH="$HOME/.local/opt/weavec-X.Y.Z-linux-x86_64/bin:$PATH"
weavec-cc --version
```

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

Other presets are `dev-asan`, `dev-tidy`, `release` and `relwithdebinfo`; see [`CMakePresets.json`](CMakePresets.json) for the full list and the [developer guide](docs/development.md) for testing, debugging and tooling. The `release` preset builds a self-contained toolchain: `cmake --install build/release --prefix <dir>` lays it out, and `cpack` in `build/release` packs the release archive. Releases also carry the source archive, `weavec-X.Y.Z-source.tar.gz`, and `SHA256SUMS`; release notes are in [CHANGELOG.md](CHANGELOG.md), and the [release workflow](docs/development.md#releasing) describes how versions are selected and published.

## Documentation

| Topic | Where |
| --- | --- |
| Installation, first check, guides | [weavec.com](https://weavec.com) |
| Command-line reference | [docs/pages/reference/cli.md](docs/pages/reference/cli.md) |
| Guarantees and scope | [docs/pages/reference/guarantees.md](docs/pages/reference/guarantees.md) |
| Safe C | [docs/pages/guides/safe-c.md](docs/pages/guides/safe-c.md) |
| Unsafe regions | [docs/pages/guides/unsafe.md](docs/pages/guides/unsafe.md) |
| Architecture | [docs/architecture.md](docs/architecture.md) |
| Design decisions (RFCs) | [docs/rfcs/](docs/rfcs/) |
| Developer guide | [docs/development.md](docs/development.md) |
| Roadmap | [docs/roadmap.md](docs/roadmap.md) |

## Repository layout

```
include/weavec/   Public C++ headers
  Core/           The library table's interface (no Clang/LLVM)
  Frontend/       The guard passes, Safe C's checker, unsafe regions, the enforcement ledger,
                  the compiler driver
lib/              Implementations, mirroring include/ (lib/Core/LibrarySpec.txt is the library table)
runtime/          The C runtime: the shadow memory, the arena allocator and its sweep, the guards'
                  slow paths, frames and globals, checked library calls, ASan's interface, reports
                  with their stacks and symbolizer
tools/weavec-cc/  The drop-in compiler driver (Clang's driver with WeaveC inside; weavec-c++ links to it)
resources/        weavec.h, the C-facing header with WEAVEC_UNSAFE and Safe C's annotations
                  (installed to lib/weavec/include)
unittests/        GoogleTest unit tests
test/             lit + FileCheck integration tests (test/Guards pins the passes and the runtime)
  cases/          Executable C cases by feature, with markers, run by scripts/run-cases.py
  corpus/         Real projects pinned by SHA, run by scripts/corpus-gate.py
scripts/          Test runners, the corpus gate, hygiene and release tooling
docs/             Architecture, RFCs (docs/rfcs/), roadmap, and the weavec.com site
cmake/            Build-system modules
```

`Core` must not include anything from `clang/` or `llvm/`. The runtime is C and includes no WeaveC header but its own. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md) first. Report security issues privately, as described in [SECURITY.md](SECURITY.md).

## License

WeaveC is licensed under the Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
