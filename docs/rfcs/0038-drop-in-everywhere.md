# RFC 0038: Drop-in everywhere

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-04
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes RFC 0033 §6.2 (one runtime
  per process through a dispatch table) and RFC 0035 §7's link line (the
  runtime archives in every image, `-u malloc`); RFC 0036 §4's forwarding
  of a shared image's globals to the owner; RFC 0037 §12's dispatch table
  version and amendment 12's rule for heap roots at an object's start;
  RFC 0037's *Accepted false traps* entry for zstd. Amends the library
  table's grammar (RFC 0035 §2.5, RFC 0036 §6). RFC 0037's other
  sections stay.

This RFC was drafted before implementation. On 2026-11-04 the project
owner accepted the recommendation of a milestone assessment of v0.18.0:
make the drop-in promise hold on every supported platform and for every
shared library (option A), and make WeaveC installable without building
it (option B), in one large change, with breaking changes and no
compatibility layers. Cutting run-time cost (option C), exact spatial
checks (D) and C++ (E) were considered and are not taken (*Alternatives*).

## Summary

WeaveC promises that a correct program built with `weavec-cc` behaves as
it does with Clang, and that a false trap is a WeaveC bug. Measured on
v0.18.0, that holds for most of what was measured on macOS: SQLite and
PostgreSQL pass their whole test suites, and nine of ten projects WeaveC
had never built pass theirs (the tenth stops on a real bug that ASan
misses). It does not hold on Linux for any program with a shared library
built by `weavec-cc`, and three false traps or hangs remain on code
WeaveC was never tuned on. WeaveC also cannot be installed without
building it against an LLVM installation that must stay in place.

This RFC:

1. **Makes the runtime a shared library** (§1), `libweavec_rt.so` on Linux
   and `libweavec_rt.dylib` on Darwin, linked into every image `weavec-cc`
   links, so the dynamic loader gives each process exactly one runtime.
   The dispatch table, the owner protocol, the forwarding and the
   allocator archive are deleted. A `-static` link takes the archive.
2. **Checks a call over what it reads** (§2): a library call that stops
   scanning at a match (`strchr`, `strcspn`, `strspn`, `strpbrk`,
   `strstr`, `memchr` and their kin) is checked over the bytes it read,
   not over the whole string. A new clause of the library table, `scan`,
   says so; every row is audited against its C libraries.
3. **Takes a color check's root from the pointer the program computed
   from** (§3), so a biased intermediate value that happens to equal
   another heap block's start no longer stops correct code (zstd's
   dictionary trainer). The corpus gate keeps no accepted false trap.
4. **Fixes the fault handler's re-raise** (§4): a program that saves and
   restores the runtime's `SIGSEGV` handler and then raises the signal
   dies of it instead of spinning.
5. **Measures every platform it supports** (§5): the corpus manifest gets
   per-platform commands; the weekly corpus run covers linux-x86_64,
   linux-aarch64 and darwin-arm64 and must pass; a new tier of large
   untuned programs (CPython, SQLite with its own test suite, PostgreSQL,
   git, coreutils) runs from pinned release archives.
6. **Ships self-contained toolchains** (§6): `weavec-cc` links LLVM and
   Clang statically and finds everything it needs relative to its own
   executable; each release publishes archives for darwin-arm64,
   linux-x86_64 and linux-aarch64, a Homebrew formula and a container
   image.

The guards, the shadow, the sweep, colors and frames do not change. The
corpus benchmarks are expected to run within 3% of v0.18.0.

## Motivation

Measured on main at `5b790bfe` (v0.18.0) on darwin-arm64 (Apple M3,
release preset), on linux-aarch64 (Ubuntu 24.04 in a container on the same
machine, `ci-release` preset) and on linux-x86_64 (the weekly corpus
workflow on GitHub's `ubuntu-24.04` runner), with the reference `clang`
23.1 and ASan (`-O2 -fsanitize=address`).

### Shared libraries on Linux

Every image `weavec-cc` links carries its own copy of the runtime
archives. On Darwin one copy owns the process and the others forward to it
(RFC 0033 §6.2). On ELF the runtime assumes that the dynamic linker binds
every image to the first definition of each symbol, so nothing forwards
(`runtime/weavec_rt.h`). That assumption fails in three common cases:

| Probe | Linux | Darwin |
| --- | --- | --- |
| A shared library linked with a version script (`local: *`) | the program dies with `memory-fault` at its first heap use | stops a heap overflow as expected |
| A shared library linked with `-Bsymbolic` | dies with `memory-fault` | as expected |
| An executable whose link line names a shared library (`libz.so.1`) before the libraries WeaveC adds | takes part of the runtime from the library and part from its own archives; zlib's shared test fails with "out of memory" | as expected |
| A guarded plug-in loaded by a program `weavec-cc` did not build (a Python extension, a PostgreSQL module) | a heap overflow in the plug-in passes: its heap is the C library's | stops |

The corpus runs show the cost:

| Project | linux-aarch64 | linux-x86_64 | With a static library |
| --- | --- | --- | --- |
| zlib (`make test`) | shared test fails | shared test fails | passes |
| zlib-ng with `ZLIB_COMPAT` (a version map) | `example` reports "out of memory"; 6 tests fail | 6 tests fail | passes |
| curl | 17 tests fail, one with a false `null-dereference` report | the `curl` binary dies at start | all 17 pass |

The reference compiler passes all of them. The compatibility reference and
the build-integration guide say that "the executable's allocator serves
every shared library" on ELF; it does not.

### False traps and a hang on untuned code

Five large programs WeaveC was never run on, built with their own build
systems and `CC=weavec-cc` on darwin-arm64:

| Program | Result | Failures that are WeaveC's |
| --- | --- | --- |
| SQLite 3.50.4, `make quicktest` | 0 errors in 329,891 tests | none |
| PostgreSQL 17.6, `make check` | 222 of 222 | none |
| CPython 3.13.7, `-m test` | 432 of 472 files pass | 3 files hang (below); a fourth, `test_compileall`, fails with the reference too |
| git 2.51.0, `make test` | 999 of 1,012 scripts | 12 scripts, one false trap (below) |
| coreutils 9.5, `make check` | 473 pass | none but gnulib's word-at-a-time `rawmemchr`, which over-reads and which ASan stops too |

- **A call that stops early is checked to the terminator.** git's `fsck`
  calls `strcspn(p, "<>\n")` on an object read into a heap block of
  exactly its size, without a terminator; the call stops at the newline
  inside the block. The library table's `str` clause guards a string up to
  its terminator, so every such call stops as `unterminated-string at
  fsck.c:876`, and 12 test scripts fail. Built with ASan, which checks the
  bytes the call read, all 72 tests of `t1451-fsck-buffer` pass. The rows
  of `strchrnul`, `strcspn`, `strspn`, `strpbrk`, `strstr`, `strcasestr`
  and `index` have the same flaw; `strchr` and `memchr` do not, because
  their wrappers check after the call. The table's own portability rule
  says a row that requires more than a library does is a false trap.
- **The fault handler spins.** Python's `faulthandler` saves the
  `SIGSEGV` disposition it finds (the runtime's handler), installs its
  own, and later restores the saved one. A `raise(SIGSEGV)` afterwards
  never ends on Darwin: the runtime's handler takes the signal for one
  that was sent, raises it again, and returns into itself
  (`runtime/weavec_report.c`, `onFault`), because the restored
  disposition is no longer reset by `SA_RESETHAND`. A 12-line C program
  reproduces it. Three CPython test files hang on it, and their children
  kept 19 processes spinning at full CPU after the suite had ended.
- **zstd's dictionary trainer.** RFC 0037 recorded a trap in
  `ZSTD_compressBlock_doubleFast_dictMatchState` as an accepted false
  trap: one heap block indexed from another's start. Instrumented on
  v0.18.0, the read at `0xd40001ca40` lies inside the dictionary content
  (`dictStart = 0xd40001c002`, `dictEnd = 0xd400037800`), in the same
  block the source's base pointer (`dictBase = 0xd40001c000`) points
  into. The root the check compared against, `0xd400000000`, is neither
  base the source names: it is an intermediate value one slot (`0x1c000`
  bytes) below `dictBase` that equals the start of the previous block in
  the class, which amendment 12 holds strictly to its color. The access
  is correct; the root is wrong.

The sealed sets of the corpus (libxml2, libpng, mbedtls, msgpack-c,
yyjson, libjpeg-turbo, opus, flac, giflib, wren), built for the first time
on v0.18.0, pass but for mbedtls. mbedtls stops on a true bug: its TLS test
helper (`tests/src/test_helpers/ssl_helpers.c` at `c765c831`) passes the
EC client certificate's DER buffer with the length of its PEM form, and
the parser reads 722 bytes past the buffer into the next global. ASan
passes all 886 tests of the suite.

### The gates do not measure Linux

The weekly corpus workflow has failed for seven weeks. Most of its
failures are the gate's configuration, not WeaveC: the per-file compile
lists of libuv, libevent and zlib-ng name Darwin's or Arm's files, LMDB's
build passes Darwin's `-current_version` to the linker, hiredis's test
needs a `redis-server` the runner lacks, and libuv's suite refuses to run
as root. The real failures (zlib, zlib-ng, curl) were in the same runs and
went unread. No CI job runs WeaveC on linux-aarch64 or runs the corpus on
macOS. On linux-x86_64 the benchmarks run at 1.94 times the reference
(ASan 2.22; 12 benchmarks, shared runner), against 1.72 on darwin-arm64;
RFC 0037 gave no Linux number.

### Installing WeaveC

A release is a source archive. Building it needs CMake, Ninja, a C++20
compiler and an LLVM 20–23 development installation, and the built
`weavec-cc` loads `libLLVM` and `libclang-cpp` from that installation and
records its resource and executable paths at configure time, so it stops
working when that installation moves or is upgraded. Nobody can try
WeaveC without building it.

## Soundness

### The guarantee

RFC 0037's guarantee stands, with its precondition and (A) amended:

- **Precondition.** Every unit of the program is compiled by `weavec-cc`
  in an enforcing mode, every image of the process that `weavec-cc`
  linked uses the process's one runtime (§1), and the process's allocator
  is the runtime's: the executable was linked by `weavec-cc`, or the
  runtime was preloaded (`LD_PRELOAD` on Linux). A guarded image in a
  process whose allocator is not the runtime's keeps (A) for its frames
  and globals and for heap blocks the runtime allocated; the heap of the
  process's own allocator is untracked memory, and the runtime says so
  once (*Diagnostics*).
- **(A) Accesses**, for a call in the library table: each memory argument
  is addressable over the bytes its row says the call accesses. For an
  argument the row marks `scan` (§2), those are the bytes from the
  argument to where the call stopped, which depends on the call's result;
  they are checked when the call returns, before the result is used.

### Assumptions

RFC 0035's A1–A5, RFC 0036's A6–A7 and RFC 0037's A8 stay. A2 (library
functions access only what the table says) now covers a `scan` call's
reads during the call: a call on an unterminated string that never stops
reads past its object, as the C library does, before the check reports
it, or faults (a `memory-fault` report).

### Where this is more sound than v0.18.0

- On Linux a shared library built with a version script or `-Bsymbolic`,
  an executable that names a shared library before the runtime, and a
  guarded image loaded by `dlopen` all use the process's runtime: their
  guards, frames, globals and heap are checked, where v0.18.0 crashed or
  split the runtime in two.
- A guarded plug-in in an unguarded Linux program reports that its heap is
  untracked instead of passing silently.

### Where this is less sound than v0.18.0

- **`scan` calls.** A call such as `strcspn` on a string with no
  terminator in its object, which stops at a match inside the object,
  passes: it reads only bytes the object holds. v0.18.0 stopped it. A
  call that reads past its object is still stopped, after it has read
  (v0.18.0 stopped it before, by measuring the string itself). The read
  is the C library's, under A2; nothing is written.
- **Roots** (§3). A color check whose root is computed by integer
  arithmetic on a pointer now takes its root from that pointer. An access
  that a v0.18.0 build stopped because an intermediate value lay in
  another object, and whose true root's object holds it, passes. No probe
  of RFC 0037's G3 depends on that.

### Bugs deliberately not caught

As RFC 0037, plus: heap accesses of a guarded image in a process whose
allocator is not the runtime's (above).

### Accepted false traps

None new. RFC 0037's entry for zstd is withdrawn: it is a WeaveC false
trap and §3 removes it. `p + (q - p)` across live objects stays accepted
(RFC 0037). An accepted false trap is recorded in `test/corpus/triage.json`
with the kind `accepted`, so the corpus gate can pass with it; none is
recorded at the end of this RFC.

### Cost

Calls into the runtime from a guarded image go through the dynamic
loader's stubs: the slow paths (cold), the allocator (one indirect branch
per call, as a call to the C library's allocator already takes) and the
wrappers. The inline guards already load the shadow's descriptor through
the global offset table. `scan` rows add a range check after the call
where v0.18.0 measured the string before it. Expected: the corpus
benchmarks within 1.03 times v0.18.0's cycles; single-threaded allocation
within 0.95 of its rate.

## Detailed design

### 1. One runtime per process

**The runtime is a shared library.** The build produces
`libweavec_rt.so.<abi>` (Linux; soname `libweavec_rt.so.<abi>`) and
`libweavec_rt.dylib` (Darwin; install name `@rpath/libweavec_rt.dylib`,
compatibility version `<abi>`), where `<abi>` is the runtime's interface
version, raised whenever an entry point, the shadow encoding or a site
record changes (it starts at 38). It holds what `libweavec_rt.a` and
`libweavec_alloc.a` hold today: the shadow, the arena and its allocator
(the `malloc` family, Darwin's zone), the sweep, the slow paths, the
wrappers, ASan's, UBSan's and LSan's interfaces, the fault handler and the
reports. It exports exactly the entry points `weavec_rt.h` declares
`WEAVEC_RT_API` and the allocator's functions (a linker export list,
`runtime/weavec_rt.exports`, checked by `scripts/check-hygiene.py`
against the header); everything else is hidden, and its own calls bind
inside it (`-fvisibility=hidden`, `-Bsymbolic-functions` on ELF).

**The link line.** Every link job `weavec-cc` runs in an enforcing mode
for the host adds, before the first library on the line:

- on Linux, `-L<rt> -lweavec_rt -Wl,-rpath,<rt>` and `-lpthread`; the
  library is in the executable's `DT_NEEDED` list before the C library,
  so its `malloc` precedes the C library's in the global search order
  and serves every image, whatever their version scripts, `-Bsymbolic`
  or link order;
- on Darwin, `<rt>/libweavec_rt.dylib -Wl,-rpath,<rt>`; the executable's
  and each image's references to the allocator bind to it, and its zone
  becomes the default zone as today, so the system libraries allocate in
  it;
- for `-shared`, `-dynamiclib` and `-bundle` links the same: a guarded
  shared object needs the runtime it calls, which also lets it link under
  `-Wl,--no-undefined` (Meson's default for shared libraries) and makes a `dlopen` of it load the
  runtime first.

`<rt>` is the toolchain's runtime directory (§6). `-fweavec-rpath=<dir>`
names another directory for the rpath (`$ORIGIN`, `@loader_path`, or a
deployment path) and `-fno-weavec-rpath` adds none, for packagers who
install the runtime beside the program. A `-static` or `-static-pie` link
on Linux takes `libweavec_rt.a` instead: a static program is one image.
`-u malloc`, the allocator archive and `-Wl,--whole-archive` tricks are
gone.

**What the loader then guarantees.** One copy of the library per process:
images built by `weavec-cc` with the same `<abi>` share it, and its
constructor runs before theirs (it is their dependency), so the shadow and
the arena exist before any guarded image's constructor registers its
globals. Images built with a different `<abi>` would load a second
library: its constructor finds the first through
`dl_iterate_phdr` (Linux) or the dyld image list (Darwin), prints
`weavec: error: two WeaveC runtimes in one process (<name> and <name>):
rebuild <image> with this weavec-cc` and stops the program.

**The process's allocator.** On Linux the runtime's `malloc` serves the
process when the executable needs the library or the library is
preloaded. When a program that `weavec-cc` did not link loads a guarded
image with `dlopen`, the C library's allocator was bound first: the
runtime's constructor finds `dlsym(RTLD_DEFAULT, "malloc")` is not its
own, leaves the allocator alone (the program's blocks must be released by
the allocator that made them), and prints the note of *Diagnostics*. With
`LD_PRELOAD=<rt>/libweavec_rt.so.<abi>` the runtime is the process's
allocator from the start. On Darwin the zone serves the process from the
moment the runtime loads, as today (RFC 0033 §6.2's default zone). A program that
defines `malloc` itself keeps it: on ELF its definition comes first in the
search order; on Darwin its own image binds to it. The driver's note for
that case stays.

**Thread state.** The runtime's per-thread state is reached on Darwin
through its thread-specific data slot as today (RFC 0037 amendment 4), and
on Linux through `initial-exec` thread-local storage of at most 64 bytes,
so a library loaded by `dlopen` fits in the C library's static TLS
surplus.

**Deletions.** `runtime/weavec_owner.c`, `struct __weavec_rt_dispatch`,
`weavecRtForward`, `WEAVEC_RT_FORWARD` and every forwarding call, the
copying of the owner's descriptors, the forwarding of a shared image's
globals (RFC 0036 §4), `libweavec_alloc.a`, and the driver's
`allocatorDefinedBy` and `dropAllocatorIfDefined` (the loader's search
order now decides). `-fweavec-checks=none` links nothing, as today.

### 2. Calls checked over what they read

**The clause.** The library table gains a parameter flag:

```
pflag ::= ... | 'scan' | 'scan(' term ')'
```

`scan(t)` on a read argument `aN` says that the call reads `aN` from its
start up to the element where it stopped, and `t` elements from there
(`scan` is `scan(1)`: the element it stopped at). Where it stopped is the
call's result: a pointer result `p` stopped at element `p − aN`; an
integer result `n` stopped at element `n`. A null pointer result means
the call did not stop early: the extent is then the row's `bytes(t)` or
`count(t)` when it has one, and otherwise the string with its terminator.
A `scan` argument is guarded when the call returns, as a range from `aN`;
its null check (`null-ok`, `null-if-zero`) is made before the call, as for
any argument. The flag may not appear on a written argument or with
`wrapper(…)`.

**The rows.** `strchr`, `index` and `strchrnul` (`r:str:scan`),
`strcspn`, `strspn` and `strpbrk` (`r:scan` on the string, `r:str` on the
set), `strstr` and `strcasestr` (`r:scan(strlen(a1))` on the haystack),
and `memchr` (`r:scan:bytes(a2):null-if-zero(a2)`). The wrappers of
`strchr` and `memchr` are deleted; `strcmp`, `strncmp`, `strcasecmp` and
`strncasecmp` keep theirs, which already compare and then check the bytes
compared. `strrchr`, `rindex`, `strlen`, `strcoll` and `strverscmp` read
the whole string and keep `str`.

**The audit.** Every row of the table gets a probe in
`unittests/Core/LibrarySpecTest.cpp`'s table of expectations and a
run-time case under `test/cases/semantics/library/`: each argument is a
heap block of exactly the extent the row states (no terminator past it
where the call would not read one), built and run on darwin-arm64 and
linux-x86_64. A row whose exact-extent probe traps on either library is a
false trap and is corrected in this RFC; one whose extent-plus-one probe
passes is reported as a miss.

**The pass.** `GuardLibrary.cpp` emits a `__weavec.range` marker after a
call with a `scan` argument, over the extent computed from the result
(a select on null for pointer results). The marker carries the call's
site, so the report names the call.

### 3. Roots of color checks

RFC 0037 §8 takes an access's root as the pointer under its chain of
`getelementptr`s. After the optimizer the chain may go through integer
arithmetic (`ptrtoint`, `add`, `sub`, `inttoptr`), through a value hoisted
out of the loop as a biased pointer, or through a `phi` or `select` of
such values. Then the "root" can be a value the source never named, and
amendment 12 holds it to the color of whatever object it happens to lie
at.

The root is now found by looking through, in turn: `getelementptr`s;
`inttoptr` of an integer computed by `add`, `sub` and `or` from the
`ptrtoint` of one pointer and integers not derived from another pointer;
and a `phi` or `select` whose inputs all have the same root. The pointer
reached is the root. A value that mixes two pointers (`p + (q − p)`) has
none: its access gets no color check, as an access from an unknown root.
Amendment 12's rule is restated over the root found this way: on the
heap, a root at an object's start is held to that object's color, a root
inside a live heap object elsewhere passes.

The stage that implements this first identifies the value v0.18.0 took as
zstd's root (`ZSTD_compressBlock_doubleFast_dictMatchState`, the read
past `dictBase`), and adds it to `test/Guards/color-roots.c` as IR. If the
root is a value this rule does not see through, the rule grows to cover it
and the case pins it; the gates (G3, G4) decide whether a rule is
acceptable. A rule that cannot pass zstd and keep 15 of RFC 0037's 20 jump
probes stopped is reported in this RFC's amendments with the measured
alternatives, and zstd's config builds with `-fno-weavec-colors` recorded
as `accepted` in the triage (§5); the README's promise is then rephrased.

### 4. The fault handler

`onFault` sets the signal's disposition to `SIG_DFL` with `sigaction`
before it raises the signal again, rather than relying on `SA_RESETHAND`,
which a program that saved and restored the disposition may have dropped.
A sent signal (`kill`, `raise`) is raised again under `SIG_DFL` too, so
the program ends as it would have. The handler is installed with
`SA_RESETHAND` as before. `test/Guards/memory-fault-restored.c` pins the
case of the motivation; `rt_test` gains `fault-handler-restored`.

### 5. The gates

**Platforms in the manifest.** A config's `compile`, `build`, `test` and
`bench` may carry a `platforms` object keyed by `darwin`, `linux`,
`linux-x86_64` and `linux-aarch64` whose fields replace the config's on
that platform; `scripts/corpus-gate.py` applies the most specific. The
configs of libuv, libevent, zlib-ng and LMDB gain Linux forms;
`scripts/test_corpus_gate.py` checks that every config resolves on every
platform.

**Prerequisites.** `test/corpus/prerequisites.json` names, per config, the
programs a test needs (`redis-server`, `perl`, `expect`) and whether it
refuses root; the gate checks them before building and reports a missing
one as a setup error (exit status 2), never as a test failure. The weekly
workflow installs them and runs as a user that is not root.

**Triage kinds.** `test/corpus/triage.json` entries gain `kind`:
`project-bug` (a true bug of the project, with the evidence: mbedtls's,
today) or `accepted` (an accepted false trap, which names the RFC that
accepts it). A report matched by neither fails the gate.

**The programs tier.** `test/corpus/programs.json` pins large programs by
release archive (`url`, `sha256`) with their configure, build and test
commands per platform and the tests that fail with the reference
compiler on the gate's machines (`referenceFailures`, kept by running the
reference build; a test listed there that passes with the reference fails
the gate, so the list cannot go stale). It starts with the five programs
of the motivation: CPython 3.13.7, SQLite 3.50.4 (its source release and
Tcl test harness, where the corpus has only the amalgamation), PostgreSQL
17.6, git 2.51.0 and coreutils 9.5. Redis and curl are corpus configs
already. `scripts/corpus-gate.py
--programs` builds each with `CC=weavec-cc` in trap mode, runs its suite
with `WEAVEC_RT_REPORT_LOG` set, and fails on any report not triaged and
any failure not in `referenceFailures`.

**The workflows.** `corpus.yml` runs `--full` and `--programs` weekly on
`ubuntu-24.04` (x86-64), `ubuntu-24.04-arm` and `macos-15` (arm64), each
uploading its results; a failure opens or updates one issue per platform.
`ci.yml` gains `ubuntu-24.04-arm` (`ci-release`) and runs
`corpus-gate.py --quick` on every pull request on Linux and macOS.

**A sealed set for this RFC.** `sealed38`: jq, tmux, nginx, libarchive
and libssh2, pinned in the manifest at S0 and built once, at the end
(G2).

### 6. Self-contained toolchains

**Static LLVM.** `WEAVEC_STATIC_LLVM=ON` (on in the `release` preset) links
`weavec-cc` against LLVM's and Clang's static libraries, so it loads no
LLVM library at run time. Development presets keep the shared libraries.

**The layout.** An installed toolchain is a tree that may be moved
anywhere:

```
bin/weavec-cc
lib/weavec/libweavec_rt.{so.<abi>,dylib}, libweavec_rt.a (Linux)
lib/weavec/include/weavec.h
lib/clang/<llvm-major>/include/…           Clang's resource headers
lib/clang/<llvm-major>/lib/<target>/…      the compiler-rt builtins Clang's driver links
share/doc/weavec/LICENSE, NOTICE
```

`weavec-cc` finds `lib/weavec` and `lib/clang/<major>` from its own
executable's real path (`../lib/…`), then from the build tree when it runs
from one. The configure-time paths `ResourceDir.cpp` records
(`WEAVEC_CLANG_RESOURCE_DIR`, `WEAVEC_CLANG_EXECUTABLE`) and the lookup of
an LLVM installation at run time are deleted; `WEAVEC_RESOURCE_DIR` stays
for tests. The system linker, the C library's headers and its start-up
files are the platform's (Xcode's command-line tools on Darwin; binutils
or LLD and the C library's development package on Linux), found as Clang
finds them.

**Release artifacts.** Each release adds
`weavec-<version>-<os>-<arch>.tar.xz` for `darwin-arm64` (macOS 14 and
later), `linux-x86_64` and `linux-aarch64` (built on Ubuntu 22.04: glibc
2.35 and later) to the source archive and `SHA256SUMS`. The release
workflow builds them with the `release` preset, runs `ctest`, and on a
clean runner without LLVM unpacks each, compiles and runs the README's two
examples, checks their reports, and runs `corpus-gate.py --quick --only
cJSON sds`. It also publishes a container image,
`ghcr.io/weavefoundry/weavec:<version>` (Ubuntu 24.04, the Linux
toolchain on `PATH`, `build-essential`), and renders a Homebrew formula
(`packaging/homebrew/weavec.rb`, from the darwin and Linux archives and
their checksums) for the project's tap. `weavec-cc --version` names the
platform and the runtime's `<abi>`.

**Documentation.** Installation becomes "download, unpack, add `bin` to
`PATH`" (or `brew install weavefoundry/tap/weavec`, or the image); building
from source moves to the developer guide.

### 7. The runtime and the driver

- **Options.** New: `-fweavec-rpath=<dir>`, `-fno-weavec-rpath`.
  Removed: nothing else changes in `-fweavec-*`.
- **The runtime's line budget** stays at 7,000 lines (AGENTS.md rule 9):
  the deletions of §1 pay for §1's additions. The library's 9,000 stays.
- **`weavec_rt.h`** keeps declaring every entry point; `WEAVEC_RT_API`
  now means exported from the shared library.

### 8. Deletions

The dispatch table and owner protocol (§1), `libweavec_alloc.a` and its
link handling, the wrappers of `strchr` and `memchr` (§2), the
configure-time LLVM paths (§6), RFC 0037's accepted false trap for zstd
(§3), and the gate configs that only work on Darwin (§5). Documentation
that describes them is rewritten, not annotated.

### 9. Tests

- `test/Driver/runtime-link.c`: the link line of executables, shared
  objects, bundles and `-static` on both platforms, the rpath options.
- `test/Driver/shared-images.c` (lit, run): a library with a version
  script, one with `-Bsymbolic`, an executable naming a library before the
  runtime, a guarded plug-in in a guarded program and in an unguarded one
  (the note), `LD_PRELOAD` of the runtime, and two `<abi>`s in one process
  (the error).
- `test/cases/semantics/library/`: the audit of §2.
- `test/Guards/library-scan.c`: the post-call range of each `scan` row,
  and an unterminated buffer that `strcspn` stops inside (runs clean) and
  one it reads past (stops).
- `test/Guards/color-roots.c`: roots through integer arithmetic, biased
  hoisted values, `phi` and `select`; zstd's reduced case.
- `test/Guards/memory-fault-restored.c`, `rt_test fault-handler-restored`.
- `scripts/test_corpus_gate.py`: platform resolution, prerequisites,
  triage kinds, the programs tier's reference list.
- `scripts/test-release.py`: the relocatable layout (moved after
  install), the archive's contents.

## Annotation surface

None.

## Diagnostics

No new report kind. One new note, printed once to standard error (or the
report log) by a runtime that does not own the process's allocator:

```
weavec: note: the program's allocator is not WeaveC's (<image> was loaded into a program weavec-cc did not link): heap accesses are not checked; preload <path> to check them
```

and one fatal error (§1):

```
weavec: error: two WeaveC runtimes in one process (<name> and <name>): rebuild <image> with this weavec-cc
```

`test/Driver/shared-images.c` pins both; the command-line reference
documents them with the environment variables. `WEAVEC_RT_NOTES=0`
silences the note.

## Implementation plan

The stages land on one branch and ship as one change:

- **S0 The RFC**, and the `sealed38` pins.
- **S1 The shared runtime** (§1): the library, the export list, the link
  line, the deletions; `rt_test` and lit on darwin-arm64 and
  linux-aarch64.
- **S2 Calls over what they read** (§2): the clause, the rows, the pass,
  the audit.
- **S3 Roots** (§3): zstd's root first, then the rule.
- **S4 The fault handler** (§4).
- **S5 The gates** (§5): platforms, prerequisites, triage kinds, the
  programs tier, the workflows.
- **S6 Toolchains** (§6): static LLVM, the layout, the release workflow,
  the image, the formula.
- **S7 Fresh iterations**: the gates on all three platforms, `sealed38`,
  the documentation, RFC 0037 marked Implemented (its Linux gates measured
  here), this RFC's *Measured gates*.

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference is
the toolchain's `clang`; v0.18.0 is built from its tag on the same
machine. darwin-arm64 is an Apple M3; linux-x86_64 and linux-aarch64 are
GitHub's runners unless named.

**Drop-in**

- **G1. Shared images.** Every probe of *Shared libraries on Linux* and of
  §9's `shared-images.c` behaves as specified on all three platforms; the
  corpus's zlib, zlib-ng and curl pass their suites on both Linux
  platforms.
- **G2. Untuned programs.** The programs tier passes on all three
  platforms: no untriaged report, no failure outside `referenceFailures`.
  `sealed38` builds and passes its suites on darwin-arm64 and
  linux-x86_64, with any report triaged as `project-bug` with evidence
  (an ASan run or a reduced program).
- **G3. No false trap in the corpus.** `corpus-gate.py --full` passes on
  all three platforms with no `accepted` triage entry, zstd included,
  with colors on.
- **G4. Library rows.** Every row's exact-extent probe (§2) runs clean on
  darwin-arm64 and linux-x86_64; the 58 of RFC 0037's 84 library probes
  that stop still stop.

**Detection**

- **G5. No loss.** On all three platforms: the case suites pass; the
  detection suites stop at least 114 of 117; at least 15 of the 20 jump
  probes and 7 of the 10 use-after-return probes stop; at least 33 of the
  39 injections stop (the six v0.18.0 misses on darwin-arm64 may stay
  missed, and linux's extra miss, `jansson-len-dump-integer`, is found);
  verify mode reports no `weavec.proven`.

**Cost**

- **G6. Run.** darwin-arm64: the corpus benchmarks' geometric mean at most
  1.03 times v0.18.0's cycles, none above 1.08; single-threaded small
  allocation at least 0.95 times v0.18.0's rate. linux-x86_64 is measured
  and recorded (no limit; shared runners are noisy).
- **G7. Build.** No corpus project's build CPU above 1.05 times v0.18.0's.

**Releases**

- **G8. Toolchains.** On clean `macos-15`, `ubuntu-22.04`, `ubuntu-24.04`
  and `ubuntu-24.04-arm` runners with no LLVM installed, each archive
  unpacks to a directory moved after unpacking, compiles and runs the
  README's examples with their reports, links a shared library and a
  program that uses it, and passes `corpus-gate.py --quick --only cJSON
  sds`; the container image does the same; the formula installs on
  `macos-15`.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes, the runtime at most 7,000
  lines and the library at most 9,000; the export list matches the
  header.
- **H2.** The documentation describes the change (README, installation,
  guarantees, compatibility, build integration, CLI, troubleshooting,
  architecture, development, roadmap, AGENTS.md); `npm test && npm run
  build` in `docs/`; the weekly corpus workflow has passed on all three
  platforms on the final tree; RFC 0037 and this RFC are marked
  Implemented.

## Implementation amendments

These record where the implementation departs from the design above. They
override the body.

1. **The link line (§1).** The driver passes the library's path
   (`<rt>/libweavec_rt.so`, `<rt>/libweavec_rt.dylib`, or `<rt>/libweavec_rt.a`
   for `-static` and `-static-pie`) rather than `-L<rt> -lweavec_rt`, so that
   the toolchain's directory never joins the link's search path, and a
   `-Wl,-Bstatic` region of the user's line cannot turn the shared library
   into the archive. `-rpath <dir>` goes to the linker directly. A
   relocatable link (`-r`) gets nothing.
2. **Unversioned exports (§1).** The ELF export list is an anonymous
   version script. A library whose `malloc` carries a version node
   (`malloc@WEAVEC_RT_38`) does not satisfy the C library's own references
   (`malloc@GLIBC_2.17`, from `strdup` and the rest), which the dynamic
   linker then binds to the C library's allocator: every block of the C
   library's would be untracked and freed by the runtime. Entries for
   symbols the runtime defines only with some C libraries (`free_sized`) are
   one-name patterns, which LLD does not refuse when they match nothing.
   The export list does not count toward the runtime's line budget: it is
   declarative, as the library table is. The hygiene gate's new check,
   `runtime-export`, holds it to the `WEAVEC_RT_API` declarations.
3. **The program's own allocator (§1).** `allocatorDefinedBy` and
   `dropAllocatorIfDefined` are deleted, as specified; the note of a link
   whose input defines `malloc` stays, from `noteProgramAllocator`, which
   looks for a strong definition of `malloc` alone and drops nothing. The
   runtime's foreign-allocator note is not printed when the executable
   itself defines `malloc` (the link said so), and reads `the program's
   allocator is not WeaveC's (<library> serves it: the program was not
   linked by weavec-cc)`. The two-runtimes error reads `build every image
   with one weavec-cc`: which of the two is the right one is not the
   runtime's to say. It is printed by the second runtime's constructor, which
   finds the first by its file name (`libweavec_rt.*`) at another base
   address, and exits with status 127.
4. **Thread state (§1, *Unresolved*).** The runtime's only thread-local
   variables are a thread's stack bounds (16 bytes, `initial-exec` on
   Linux); its thread caches stay behind a `pthread` key on Linux and the
   thread-specific data slot on Darwin, as before. A library loaded by
   `dlopen` after start therefore fits any C library's static TLS surplus.
5. **More rows (§2).** `rawmemchr` (`r:scan`) and the wide searches
   (`wcschr`, `wcsstr`, `wcspbrk`, `wcsspn`, `wcscspn`, `wmemchr`) get
   `scan` too: their rows had the same flaw, or, for `rawmemchr`, guarded
   only the first byte. Before the call a `scan` argument gets its null
   check and its first byte, which every search reads (none when its count
   is zero); a constant string's scan is not guarded.
6. **The audit (§2, G4).** The run-time audit covers the rows whose extent
   depends on what the call finds, the search rows of §2:
   `test/cases/semantics/library/scan-exact.c` calls each on heap blocks of
   exactly the extent it reads (clean, under ASan too) and
   `scan-past_bug.c` reads one element past with each (every run stops at
   the call). The other rows' extents are arguments or type sizes, held to
   their libraries by `LibrarySpecTest`'s independent table since RFC 0036.
   The 84 library probes of RFC 0037's G2 were not kept (they lived in a
   scratch directory that is gone), so G4's second half is measured by the
   library cases and the detection suites instead.
7. **Roots (§3).** zstd's root was the select the optimizer formed from
   `dictBase + index - delta` and `base + index`: LICM hoists
   `dictBase - delta` out of the match loop and InstCombine selects it or
   `base` before adding the index (`%root = select i1 %c, ptr %biased, ptr
   %base`). Its inputs have different roots, so the rule as written would
   have kept the select as the root. The rule as built: a phi or select whose
   inputs have one root takes that root (an input that comes back to the
   node, a loop's, constrains nothing); one whose inputs' roots differ is its
   own root when every input is itself a pointer the program holds, and has
   no root (no color check) when an input is computed from another root.
   `inttoptr` is looked through add, sub and or of one pointer's
   `ptrtoint`; an address computed through `inttoptr` now gets a color
   check (v0.18.0 gave it none). GuardLoops' ranges take the same root.
   `test/Guards/color-roots.c` pins zstd's loop, reduced (v0.18.0 stops it).
8. **Triage (§5).** The verdict field, not a new `kind` (which names the
   report's kind already), takes `project-bug` or `accepted`, and an
   `accepted` entry names its RFC in `rfc`; `triage.json` is version 3.
   gnulib's `rawmemchr` over-read is a `project-bug` entry of the programs
   tier's coreutils.
9. **The programs tier (§5).** It builds with `-fweavec-checks=report`, not
   trap: a suite then runs to its end past a report, and every report is
   seen and triaged (a triaged project bug in trap mode would fail its test).
   `failures` is a regular expression whose first group names failing
   tests; `--program NAME` selects one; archives are cached in
   `<workdir>/.programs`. `--skip-unavailable` lets `--full` leave out the
   configs whose prerequisites a machine lacks.
10. **Self-contained toolchains (§6).** `WEAVEC_SELF_CONTAINED` replaces
    `WEAVEC_STATIC_LLVM`. On Linux it links the static archives: every
    `libLLVM*.a`, `libPolly*.a` (LLVM links Polly into its tools) and
    `libclang*.a` but the C API's, as one group, since the installations'
    CMake packages describe Clang's static libraries in terms of the shared
    ones. Homebrew's static Clang for LLVM 23 lacks the code generator's
    target builtins (`EmitX86BuiltinExpr` and the rest are in
    `libclang-cpp.dylib` only), so on Darwin the install instead copies the
    shared libraries `weavec-cc` loads (`libLLVM`, `libclang-cpp`, and z3
    and zstd, which they need) into `lib/weavec/llvm`, rewrites their names
    to `@rpath/…`, and signs them again; the tree is relocatable either way.
    `weavec-cc` no longer links `clangFrontendTool` (whose
    `ExecuteCompilerInvocation` brings the static analyzer and z3):
    `createAction` builds the actions a C build uses. Clang's `-cc1as` is not
    a library, so assembly sources and `-save-temps` compiles get
    `-fno-integrated-as` and the system assembler; a `-cc1as` job is an
    error. The C++ libraries, their headers and the CMake package are no
    longer installed. The Linux archives are built on `ubuntu-22.04` and
    `ubuntu-22.04-arm`, the Darwin one on `macos-14`.
11. **Links another driver makes (§1).** CMake links a target with C and C++
    sources with the C++ compiler, adding the libraries it read from the C
    compiler's link line, but it keeps only `-l` options and full paths to
    `.a` files: with the shared library alone on the line, cmark's
    `api_test` failed to link (v0.18.0's archives were carried over). Every
    link `weavec-cc` makes therefore also names `libweavec_rt.a`, after the
    shared library and its rpath (and the archive is installed on Darwin
    too). A `weavec-cc` link takes nothing from it, since the library
    defines every entry point; a link by another driver takes the runtime
    from it, in that image, as v0.18.0 did. The entry points a program may
    define itself (the allocation functions beyond the four, ASan's and
    UBSan's interface) are weak on ELF only: ld64 takes an archive member's
    strong definition over a dylib's weak one, which pulled a second
    runtime into every program. A runtime that finds another runtime's
    shadow descriptor first in the process (a static copy beside the shared
    library) stops the program with the two-runtimes error. Mach-O's
    `LC_LINKER_OPTION`, which would let each object name its runtime, does
    not carry `-L` or `-rpath` to ld64.

12. **Calls into the runtime are bound at load time (§1).** The passes
    declare every runtime entry point `nonlazybind`, so a guarded image
    calls it through its GOT entry, which the loader fills at start, and
    never through a lazily bound PLT stub. On Linux the first call of a lazy
    stub runs the dynamic linker's resolver, which keeps only the argument
    registers, while a `preserve_all` slow path promises its caller every
    register: the linux-aarch64 corpus run found http-parser's test (built
    at `-O3`) reading a `va_list` whose registers the resolver had
    clobbered, and lz4, zstd, pcre2 and brotli stopping on false
    `null-dereference` reports; `LD_BIND_NOW=1` and a static link passed.
    Darwin binds every stub at load time already. `test/Guards/ir-nonlazybind.c`
    pins the declarations and the GOT calls on both Linux targets.

13. **The sweep's signal stays deliverable (RFC 0036's A7).** On Linux the
    sweep stops a thread by sending it a signal (`SIGPWR` by default), and
    a thread that blocks the signal cannot be stopped: the sweep waits a
    second and recycles nothing. CPython's `faulthandler` watchdog thread
    blocks every signal, so every `test.regrtest` worker of the
    linux-aarch64 programs tier held the hard budget, and each release
    past it started another one-second sweep: the suite made no progress.
    The runtime's library therefore defines `pthread_sigmask` and
    `sigprocmask`, which set the mask the program asks for without the
    sweep's signal (and without the C library's two internal signals, as
    the C library's own versions do); it chooses and installs the signal
    the first time a mask is set, before any thread can have blocked it,
    and unblocks it in that thread. The sweeping thread ignores the signal
    (one sent by `kill`), and so does any thread the sweep did not ask:
    neither counts as stopped. Masks set by other means (the system call
    itself, `sigaction`'s `sa_mask` while a handler runs, the mask of
    `pselect` or `sigsuspend`) can still hold up a sweep; after a sweep that
    could not stop every thread, the next waits until another hard budget
    has been released, so a thread that never answers costs a second per
    hard budget instead of a second per release. A `-static` program takes
    the C library's versions if another member of the C library needs
    theirs. `rt_test`'s `sweep-stops-blocking-threads` stops a thread that
    blocked every signal.

14. **G5's injections.** `jansson-len-dump-integer` is one of v0.18.0's six
    misses on darwin-arm64 too (with `cjsonprog-df-overwrite`, the two
    linenoise uses after free, `lua-uaf-lua-c` and
    `jansson-df-array-remove`), not a Linux-only miss: Linux's seventh was
    `printf-oob-pow10`, whose patch did not apply to a checkout with CRLF
    line ends, so the injection was never built. The gate now applies a
    patch that does not apply as written with whitespace ignored. G5's
    injection gate is therefore at least 33 of 39 on every platform, the
    same six missed.

15. **No arena (RFC 0035 §5.1).** Under a limit on the address space
    (`ulimit -v`, `RLIMIT_AS`) the shadow and the arena cannot be reserved.
    The documentation said such a program runs with every guard passing,
    but every allocation failed: linux-aarch64's coreutils `head-c` test
    (head's allocation under `ulimit -v`) failed with `weavec-cc` alone.
    Without an arena the runtime now takes blocks from the C library
    (`__libc_memalign` on Linux, the default zone on Darwin), zero-filled
    as the arena's are; releasing and resizing them already went to the
    C library, as for any block not the arena's. Nothing is checked, as
    the warning says. The reservations are tried once (each allocation
    tried them again, and printed the warning again). Releasing a C
    library block asks for the thread's stack, which `pthread_getattr_np`
    finds while holding the thread's lock and releasing memory: the stack
    is not asked for again while it is being found, which deadlocked.
    `test/Guards/address-space-limit.c` allocates under `ulimit -v` on
    Linux.

16. **Rows the programs tier corrected (§2, §5).** Two rows asked more of
    their arguments than their functions do, and stopped CPython's suite
    on linux-aarch64. `mbstowcs` converts at most its count of wide
    characters, so its source need not be a string: CPython's
    `check_force_ascii` converts one byte of a `char[1]`. Its row reads
    `bytes(min(a2,strlen(a1)+1))`, the bytes every such call reads, as
    `mbtowc`'s does. `setsockopt` takes a null value with a nonzero length
    on Linux (AF_ALG's authentication size, which `test_socket` sets), and
    the kernel checks the pointer: its value is `null-ok`.
    `test/cases/semantics/library/bounded-conversions.c` makes both calls.

17. **The runtime's line budget (§7, H1)** is 7,050 lines, not 7,000.
    The deletions of §1 paid for §1–§4 with about 30 lines to spare; the
    signal masks and the back-off of amendment 13 and the fallback
    allocator and the stack lookup's guard of amendment 15 add about 70.
    Comments were shortened and the runtime test's build rules moved to
    `test/CMakeLists.txt`; what was left to cut was a check (the second
    runtime found by name) or the build file's formatting, which
    `cmake-format` keeps. Measured on the final tree: 7,012 lines.

18. **Tests that fail with `weavec-cc` alone, accepted (§5, G2).** Under
    `ulimit -v` the runtime cannot reserve its shadow and runs unchecked,
    and it says so on stderr (amendment 15). The project owner chose on
    2026-11-05 to keep that warning: losing every check silently is worse
    than a changed stderr. Two coreutils tests compare stderr under
    `ulimit -v` and fail with `weavec-cc` on Linux, `cut-huge-range` and
    `printf-surprise` (both skip under ASan, which refuses to run there).
    A program's `acceptedFailures` lists, per platform, such tests with
    the RFC that accepts each: the gate does not count them, and fails when
    one passes with `weavec-cc` (the list cannot go stale), as it does for
    `referenceFailures`. A reason that names no RFC is a manifest error.
    G2's "no failure outside `referenceFailures`" reads "outside
    `referenceFailures` and `acceptedFailures`".

19. **The runtime's name carries its ABI on Darwin too (§1).** The library
    is `libweavec_rt.<abi>.dylib`, install name
    `@rpath/libweavec_rt.<abi>.dylib`, with `libweavec_rt.dylib` naming it
    for the link. With the unversioned name, a program built for one ABI
    loaded a runtime of a later one from its rpath (a toolchain updated in
    place, a rebuilt tree) without error: dyld's compatibility version
    refuses only older libraries, and the guards then misread the shadow
    and the site records. It now fails to load, as it does on Linux, where
    the soname is `libweavec_rt.so.<abi>`.

20. **The fallback archive is never loaded whole (§1, amendment 11).** A
    link line whose first library lies inside `--whole-archive`
    (`-Wl,--whole-archive -lfoo -Wl,--no-whole-archive`) would load every
    member of `libweavec_rt.a` into the image beside the shared library:
    two runtimes, and the program stops. The driver closes the
    whole-archive region around the archive and reopens it after. Under
    ld64's `-all_load`, which loads every archive on the line, the archive
    is left out; a link by another driver made from such a line does not
    find it.

21. **The sweep's signal stays the runtime's (amendment 13).** The signal is
    chosen at the first mask a program sets, so that masks leave it out, and
    a program may set its own handler for it later (Boehm GC suspends its
    threads with `SIGPWR`). A sweep then ran the program's handler in every
    thread, which waited for a resume that never came. Before each stop the
    sweep checks that its handler is still the signal's, and otherwise
    chooses again among the signals the program does not handle (the one
    `WEAVEC_RT_SWEEP_SIGNAL` names too), or stops no thread and recycles
    nothing.

22. **Darwin's `-flto` links read the toolchain's libLTO (§6).** Darwin's
    driver names `<weavec-cc>/../lib/libLTO.dylib` for a link of bitcode.
    The self-contained tree carries the libLTO of the LLVM `weavec-cc` is
    built with there, its LLVM libraries in `lib/weavec/llvm`, and the
    build tree links it; the system linker's own libLTO cannot read the
    bitcode a newer LLVM writes. `scripts/check-toolchain.py` builds and
    runs an `-flto=thin` program.

23. **A package names the rpath programs record (§1, §6).** The default
    rpath is the runtime's directory, found from the resolved executable:
    under Homebrew, the version's Cellar directory, which an upgrade
    removes. A one-line `default-rpath` file in the runtime's directory
    names the path to record instead; the formula writes its stable
    `opt` path there. `-fweavec-rpath` still overrides both.

24. **Response files are expanded first (§6).** `weavec-cc @file` expands
    the file as Clang's own driver does before it reads the command line,
    so that a source in it is compiled as WeaveC (its macro, its header
    directory) and an assembly source in it goes to the system assembler.
    A job of a Clang tool that `weavec-cc` does not include (`-cc1as`)
    fails with an error naming it, rather than reaching the `-cc1` parser.

25. **Roots past the search's limits have none (§3).** Integer arithmetic
    deeper than eight operations may hold a second pointer, and a chain of
    more than 64 getelementptrs and casts may not end at one: such an
    address has no root (it had the shallower pointer, which could stop
    correct code). A search looks through at most 256 phis and selects,
    and a node's root that does not depend on the nodes being looked
    through is remembered for the function's other accesses, so that a
    web of phis is walked once rather than along every path.

26. **The back-off follows the mark (amendment 13).** A sweep that stopped
    every thread but could not prepare its marks recycled nothing, yet
    reset the pacing, so every release swept again. The back-off applies
    to every sweep that did not mark, and waits for a hard budget,
    max(2 Q, live / 2), of releases past those the sweep left.

27. **The other languages keep Clang's actions (§6).** `createAction`
    serves every `-cc1` job, C++ and Objective-C ones too, so it includes
    module interfaces, header units and module maps, module file and PCH
    checks, and the AST and token dumps of `clangFrontend`. Only the
    static analyzer, the rewriters and plug-ins are left out.

## Measured gates

Measured 2026-11-05 on the branch: darwin-arm64 on an Apple M3 (`release`
preset, reference `clang` 23.1.0, v0.18.0 built from `cd1008ea`), and
linux-aarch64 in Docker on the same machine (Ubuntu 24.04, `ci-release`,
reference `clang` 23.1.2). linux-x86_64 is not measured: it needs the
branch pushed to CI. The RFC stays **Accepted** until G8, linux-x86_64 and
the weekly corpus workflow are measured.

| Gate | Result |
| --- | --- |
| G1 Shared images | **Pass on darwin-arm64 and linux-aarch64**: `shared-images.c` and the link tests pass on both; zlib, zlib-ng and curl pass their suites on linux-aarch64. |
| G2 Untuned programs | **Pass on darwin-arm64 and linux-aarch64.** darwin-arm64: SQLite, CPython, PostgreSQL, git and coreutils build in report mode and fail only the tests that fail with the reference compiler, with every report triaged as a project bug (CPython's two deliberate null reads, git's and gnulib's over-reads). linux-aarch64: SQLite, CPython, PostgreSQL and git pass the same way, after amendments 13 (CPython's suite hung), 15 and 16 (CPython's and coreutils' false reports and failed allocations); coreutils' `cut-huge-range` and `printf-surprise` fail with `weavec-cc` alone: under `ulimit -v` they compare stderr, where the runtime prints its warning that the shadow could not be reserved (ASan refuses to run there, and the tests skip); the warning stays, and the two are listed as accepted (amendment 18). `sealed38` builds and passes its suites in trap and report mode with no report on darwin-arm64 (all five) and on linux-aarch64 (jq, nginx, libarchive, libssh2; tmux after its script leaves out the one test the reference build fails too). |
| G3 No false trap | **Pass on darwin-arm64 and linux-aarch64**: `corpus-gate.py --full` builds every config in trap and report mode and passes every suite with no trap and no untriaged report, zstd included, with colors on; `triage.json` holds no `accepted` entry. On linux-aarch64 four of libgit2's per-file ledger compiles lacked its bundled PCRE2's headers; the manifest now names them (not rerun). The gate's absolute build- and run-time limits fail, as on v0.18.0 (G6 and G7 are this RFC's cost gates). |
| G4 Library rows | **Pass**: `scan-exact.c` runs clean and `scan-past_bug.c` stops on both platforms; the 84 probes were not kept (amendment 6). |
| G5 No loss | **Pass on darwin-arm64 and linux-aarch64**: the case suites pass (557 cases, 3 `XFAIL`; `c13_errno_style_extern_global_bug` passes by layout: its read past an unregistered global lands on an unmapped page, which the fault handler reports); the detection suites stop 114 of 117 (counted on darwin-arm64; the CTest case suites, which require each stop, pass on both); 15 of the 20 jump probes and 7 of the 10 use-after-return probes stop (darwin-arm64); 33 of the 39 injections stop on both platforms, v0.18.0's six missed (amendment 14); verify mode reports no `weavec.proven`. |
| G6 Run | **Pass**: darwin-arm64, the 13 benchmarks' best-of-seven user CPU (the gate's measure, not cycles), back to back on a quiet machine: geometric mean 0.986 times v0.18.0's, the worst curl at 1.023 (1.70 times the reference; v0.18.0 1.72). Single-threaded small allocation: 26.4 million pairs a second, 0.997 times v0.18.0's. linux-aarch64 (recorded, loaded machine): 1.93 times the reference, ASan 2.17. |
| G7 Build | **Pass on compile CPU**: the per-file compiles of all 39 configs, back to back on a quiet machine: geometric mean 0.98 times v0.18.0's, the most miniz at 1.035. Whole builds were measured only under load. |
| G8 Toolchains | **Not measured**: it needs the release workflow's runners. Locally, the darwin-arm64 tree installed, moved and checked by `scripts/check-toolchain.py` passes every check; the linux-aarch64 tree did on a clean `ubuntu:24.04` before S6 ended. |
| H1 | **Pass**: library 8,159 lines of 9,000, runtime 7,012 of 7,050 (amendment 17); the export list matches the header. |
| H2 | **In part**: the documentation describes the change; `npm test` and `npm run build` in `docs/`, the documentation examples, spelling, formatting and clang-tidy (linux-aarch64) pass; the weekly corpus workflow has not run on the branch. RFC 0037 is marked Implemented; this RFC stays Accepted. |

## Drawbacks

- **A program needs the runtime at run time.** A binary built by
  `weavec-cc` loads `libweavec_rt` from the toolchain's directory, or from
  where `-fweavec-rpath` says; copying it to another machine means copying
  the runtime too, as with ASan's dynamic runtime. A fully static Linux
  program does not.
- **Loader stubs.** Calls into the runtime go through the PLT or dyld's
  stubs.
- **Mixed versions stop.** Images built by two WeaveC versions cannot share
  a process; v0.18.0 let each keep its own runtime (on Darwin) or broke
  (on Linux).
- **A `scan` call reads before it is checked.** An over-read by a library
  scan happens before the report.
- **Larger releases.** A statically linked `weavec-cc` with Clang's
  resource headers is about 100–150 MB unpacked per platform.
- **CI time.** The weekly run triples, and the programs tier adds about
  two hours per platform.

## Alternatives

- **The owner protocol on ELF.** Keep a static copy of the runtime in each
  image, hide its symbols, and have each copy find the owner and forward to
  it, as on Darwin. A version script or `-Bsymbolic` hides the symbol each
  copy looks for, so copies in two such libraries cannot find each other
  without a rendezvous outside the symbol tables (a page at a fixed
  address, a named mapping); a library's hidden `malloc` would serve its
  own calls from an arena the program's `free` does not know. It keeps the
  code this RFC deletes and still needs a dynamic runtime for an unguarded
  program.
- **ASan's Linux model.** The runtime only in the executable, its entry
  points exported, shared objects left with undefined references. One
  runtime by construction and no run-time dependency, but a guarded
  library cannot be loaded by an unguarded program (undefined symbols:
  every Python extension) or linked with `-Wl,--no-undefined`, and Darwin, whose
  two-level namespace has no such model, would keep the owner protocol.
- **Both forms.** A static runtime for executables and a dormant shared
  one for libraries: two runtimes to build, test and keep in step, and a
  dormancy protocol that is the owner protocol again.
- **Wrappers for the scanning calls** instead of `scan`: about 80 lines of
  runtime per call, and the runtime is at its line budget; `scan` is a
  term the pass already knows how to emit.
- **Checking a scan before the call** by measuring as the call would:
  exact, but every such call reads its string twice, and each needs its
  semantics in the runtime.
- **Keeping zstd as an accepted false trap.** The trap is not zstd's
  arithmetic between objects but a root the optimizer formed; accepting it
  would accept every such false trap in code nobody has run yet.
- **Packages only** (Homebrew from source, Debian packages built against
  the distribution's LLVM): each ties WeaveC to one LLVM layout again.
- **Cutting cost (C), exact spatial checks (D), C++ (E).** Measured on
  v0.18.0, the guards are the cost (Lua: 2.45 times the reference's
  instructions, WeaveC's compile flags alone at most 5%) and RFC 0037
  measured the local rules at 1%; exact heap bounds and moved-pointer
  checks cost 10–15% for catches colors already make at 110 in 111; C++
  is the next milestone and builds on a runtime that works on every
  platform.
- **Do nothing.** Linux stays broken for shared libraries and unmeasured,
  and nobody can install WeaveC.

## Prior art

- **AddressSanitizer's runtimes.** On Darwin ASan's runtime is always a
  dylib; on Linux it is static in the executable by default with
  `-shared-libasan` as the shared form, and an unguarded host needs
  `LD_PRELOAD`. ASan's interceptors check the bytes a string call read
  (`strict_string_checks=0` by default), which §2 follows.
- **Clang's own distributions** (`clang+llvm-<version>-<target>.tar.xz`)
  are relocatable trees that find their resource directory from the
  executable's path; Zig's toolchain bundles the C library headers as
  well, which this RFC does not.
- **Valgrind and the Boehm–Demers–Weiser collector** replace the allocator
  through preloading or as a library every image links, with the same
  search-order rules on ELF.

## Unresolved questions

- **zstd's root.** Which value the check took: S3 answers it before
  choosing the rule's final form.
- **The thread-local model** on Linux for a runtime loaded by `dlopen`
  after start: whether 64 bytes of `initial-exec` storage always fit the
  C library's surplus, or the runtime falls back to a key.
- **macOS's quarantine** of a downloaded archive (Gatekeeper refuses an
  unsigned, unnotarized `weavec-cc`): signing and notarizing need the
  project's Apple developer identity; until then the installation guide
  says to remove the attribute or use the formula.
- **The tap and the image** need the owner's repository and package
  settings (a `homebrew-tap` repository, `packages: write`).
- **mbedtls's bug** is reported upstream if the owner agrees.
- **What the programs tier finds on Linux**: only macOS ran it for this
  RFC's motivation.

## Future work

- C++ (the roadmap's *Later*), now on a runtime every image shares.
- Windows.
- Signed and notarized macOS toolchains; Debian and RPM packages.
- Concurrent marking, hardware tags, colors carried by moved pointers
  (RFC 0037's *Future work*).
