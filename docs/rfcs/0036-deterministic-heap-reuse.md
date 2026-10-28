# RFC 0036: Deterministic heap reuse

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-10-07 (revised 2026-10-08)
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes RFC 0035 §5.1's quarantine
  (released blocks reused after a byte budget), §8 (the advisory analysis)
  and the analysis parts of RFC 0035 §7, §10 and §11; the analysis of RFCs
  0001–0017, 0030 §2–§9 and 0031, which is deleted; RFC 0035's *Future
  work* entry for RFC 0036 (ownership as checked contracts at function
  entry), which is withdrawn. RFC 0035's guards, shadow, frames, globals,
  library-call guards, removal rules, modes and ledger stay, amended below.

This RFC was drafted before implementation. On 2026-10-07 the project
owner accepted the recommendation of a milestone analysis and authorized
drafting this RFC and implementing it in one large change, with breaking
changes and no compatibility layers.

**History of this RFC.** Its first draft (2026-10-07, *Deterministic memory
safety*) also made spatial safety and use after return deterministic:
every access checked against the object of the pointer it was computed
from, computed pointers checked when they leave their function, and the
locals a guard can reach moved into *frame blocks* allocated from the
arena. That draft was implemented end to end on the branch
`rfc0036-deterministic-safety` and stopped at its cost gate: the corpus
benchmarks ran at 3.08 times the reference compiler (geometric mean),
twice v0.16.0's 1.53, QuickJS at 10.8. A measurement of each mechanism
(*Alternatives*) found that reuse after a sweep, the part that makes use
after free deterministic, is cheap once two fixable costs are removed, and
that provenance bounds and frame blocks are not, at any design point the
branch tried. On 2026-10-08 the owner accepted the recommendation to ship
the cheap part with the deletions and the drop-in fixes, and to leave
spatial and use-after-return determinism to a later RFC with a cost budget
as its first requirement. This revision is that scope; the first draft's
measurements are kept below.

## Summary

RFC 0035 guards every memory access against a shadow memory that says
which bytes belong to a live object. Measured on v0.16.0, the guards stop
the bugs AddressSanitizer stops, at or below its cost, and untuned
projects build and pass. But a use after free is stopped only while the
freed block waits in a 16 MiB quarantine: a program that allocates enough
after the release gets the block back for a new object, and the stale
pointer then reads and writes the new owner's data without a report. A
double free after reuse frees the new owner's block.

This RFC:

1. **Reuses a released heap block only after a sweep.** A released block
   waits in the quarantine until a *sweep* (the program's threads stopped;
   their stacks and registers, the process's writable data and every live
   heap block scanned conservatively) finds no pointer to it. A use after
   free or a double free through any pointer the program still holds traps,
   however much it allocated since. Sweeps are paced so that they take a
   bounded share of the run time.
2. **Checks a fortified call's object size.** `__strcpy_chk(d, s, 8)`, which
   Clang emits for `strcpy` into an 8-byte field when the C library
   fortifies, carries the field's size; the guards check it, where v0.16.0
   dropped it and lost an overflow inside a struct that plain Clang stops.
3. **Honours ASan's contract.** Code compiled by `weavec-cc` sees
   `__SANITIZE_ADDRESS__` and `__has_feature(address_sanitizer)`, and the
   runtime implements ASan's poisoning interface, so that code written to be
   ASan-clean (a pool that poisons its free space, an interpreter that sends
   every object to `malloc` under ASan, a scan that reads past an object on
   purpose under `#if !ASAN`) is WeaveC-clean and gets the same detection.
4. **Shares stack space between locals whose scopes never overlap**, as
   Clang does: v0.16.0 gave each local a guard can reach its own redzoned
   slot, which grew frames 5–25 times and overflowed deep recursion.
5. **Fixes what drop-in found**: globals of shared libraries and bundles get
   their redzones in the process's shadow; the `strto*` and `ato*` families
   no longer require a terminator they never read.
6. **Deletes the advisory analysis**: the `weavec` tool,
   `-fweavec-diagnose`, `weavec --whole-program`, `lib/Analysis`, all of
   `lib/Core` but the library table, every annotation but `WEAVEC_UNSAFE`,
   their diagnostics, tests and documentation. Measured on untuned code it
   reported no bug worth fixing in 69 findings and found none of the bugs
   the guards miss.

## Motivation

Measured on main at `f654555` (v0.16.0), release build, darwin-arm64
(Apple M3), with the reference `clang` 23.1.0 and ASan
(`-O2 -fsanitize=address`).

### What the guards miss

Small programs, each a bug with an attacker-controlled index or a stale
pointer (the cases of `test/cases/determinism` pin them):

| Bug | v0.16.0 | ASan | This RFC |
| --- | --- | --- | --- |
| Use after free after ~28 MiB of live allocations reclaimed the block (a function pointer called through it) | SIGSEGV later, no report | `heap-use-after-free` | `heap-use-after-free` |
| Double free after the block was reused | frees the new owner's block | passes | `double-free` |
| `strcpy(u->name, long)` into an 8-byte field followed by `int admin` | passes; `admin` overwritten | passes | `intra-object-overflow` |
| The same with plain `clang -O2` (Darwin fortifies it to `__strcpy_chk(d, s, 8)`) | | traps | |
| A global overflow inside a dylib or bundle | passes (the same code in the executable traps) | `global-buffer-overflow` | `global-buffer-overflow` |
| A pool that poisons its free space through ASan's interface, used after release | passes | `use-after-poison` | `use-after-poison` |
| Heap index from `a` that lands in a neighbouring live block `b` | passes; `b` corrupted | passes | passes (later RFC) |
| Read through a `char *` from one global into the next | passes | passes | passes (later RFC) |
| Use after return (`return &local`) | passes | `stack-use-after-return` (with its fake stack) | passes (later RFC) |

The fortified call and the dylib global are WeaveC bugs, not design limits:
the guard pass redirects `__strcpy_chk` to its own checked `strcpy` and
drops the field size, and a shared image registers its globals in a shadow
of its own (below).

### Drop-in on large untuned programs

CPython 3.13.7, SQLite 3.50.4 (shell, `speedtest1`, Tcl `testfixture`) and
GNU coreutils 9.5, none tuned for: configure results identical to the
reference compiler's; builds 1.17–1.68× the reference's time (ASan
≈1.5–2.2×). Their suites pass but for:

- **Globals of shared images are unprotected on Darwin.** The constructor
  that registers a unit's globals (priority 1) runs before the copy of the
  runtime in that image has found the process's owner, so it reserves a
  shadow and an arena of its own (about 320 GiB of address space per
  image), poisons the redzones there, and then switches to the owner's
  shadow, where they are gone. Past six extension modules CPython prints
  `the heap lies outside the shadow's window` once per module, which
  alone fails eight of its test files.
- **Frames grow 5–25×.** Every escaping local gets its own 32-byte-aligned
  slot and redzone in one frame, so locals in disjoint `switch` arms no
  longer share stack space (`obj2ast_expr` 176 → 4,352 bytes,
  `_PyEval_EvalFrameDefault` 480 → 2,240). CPython segfaults at a C
  recursion depth where the reference raises `RecursionError`
  (`test_ast`, `test_functools`).
- **`strtoumax` traps on correct input** (`tr`): the `strto*` and `ato*`
  rows require a terminated string, but the functions read only the
  number they parse and the character that stops it.

Run time stays below ASan's: `speedtest1` 1.55× (ASan 1.81×), CPython
micro-benchmarks 1.54× (4.7×), Lua 2.10× instructions (2.73×); peak memory
1.2–1.5× (ASan 2.4–6×).

### What the analysis contributes

The advisory analysis is 33,230 lines in `lib/Analysis`, 13,698 in
`lib/Core` and 1,987 in `lib/Frontend` (85% of the production C++), with
16,322 lines of unit tests and 7,138 of lit tests. On four untuned
projects (libpcap, c-ares, nginx, libssh2; 314 units) `weavec` reported 0
errors and 69 warnings; every one was read: 0 actionable, 2 true but
harmless, 5 unclear, 62 false (lists and loops taken as one object,
allocation only on success not modelled, callbacks taken to free,
ownership stored into containers). `--whole-program` took c-ares from 1 to
169 warnings and libssh2 from 2 to 70 (20 sampled: all false). On the 109
bug programs of `test/cases/detection{,-blind}` the guards stop 104, the
Clang static analyzer 36 and `weavec` 27, none of the five the guards
miss; its only findings no dynamic check can make are leaks, which the
Clang static analyzer also reports. The guard passes use none of it but
the library table.

The roadmap's next step, RFC 0036 as *ownership as checked contracts*,
would put that analysis back into what the build enforces (a contract it
infers removes a guard), which is what RFCs 0030–0034 did and RFC 0035
undid. This RFC withdraws it.

## Soundness

### The guarantee

For a program whose every unit is compiled by `weavec-cc` in an enforcing
mode (`trap` or `verify`; `report` with `WEAVEC_RT_ABORT=1`), with the
runtime linked, every memory access outside a `WEAVEC_UNSAFE` region
satisfies (A), (B) and (R) of RFC 0035, and:

- **(T) Temporal.** A byte of a released heap block is not addressable
  until a sweep has found no pointer to the block in the program's stacks,
  registers, writable data and live heap. An access through a pointer the
  program held when the block was released therefore traps, however much
  the program allocated since; a release of a block already released
  traps.
- **(F) Fortified sizes.** A fortified library call writes no more than
  the size its `__builtin_object_size` argument states, or the program
  traps.
- **(P) Poisoning.** A byte the program poisoned through ASan's interface
  is not addressable until it unpoisons it.

**What the guarantee means for the bug classes:**

- **Use after free, double free** of the heap: trap through any pointer
  the program holds in memory or registers. A pointer the program hides
  (stored as an integer it transforms, XOR-linked, written to a file or
  held only by the kernel) does not keep its block from reuse.
- **Heap, stack and global overflow**: as RFC 0035 — a contiguous overflow
  traps; an index that jumps over a redzone into another live object
  passes.
- **Use after scope**: traps, as before. **Use after return**: passes, as
  before.
- **Overflow inside an object**: by an index into a declared array (B), or
  by a fortified string or `printf` call into a field (F); not by pointer
  arithmetic.
- **Memory of a custom allocator**: tracked when the allocator poisons its
  free space through ASan's interface (P), or sends its objects to `malloc`
  when it sees `__SANITIZE_ADDRESS__`; otherwise untracked (A1).

### Assumptions

RFC 0035's A1–A5 stay, with these changes:

- **A1** (untracked memory) also covers memory the program allocates with
  its own allocator and does not poison through ASan's interface.
- **A6 — visible pointers.** A pointer to a heap block is stored, while
  the program may still use it, as a whole aligned pointer in memory the
  sweep scans (any readable and writable mapping), or in a register.
- **A7 — the runtime sees the threads.** On Linux, every thread accepts
  the sweep's signal (§3.4) within a second; a thread that blocks it holds
  up recycling (nothing is reused until it accepts), never safety.

### Where this is more sound than RFC 0035

The use after free, the double free, the fortified field overflow, the
dylib global and the poisoned pool of the motivation's table trap. The
quarantine is no longer a budget a program can exhaust.

### Where this is less sound than RFC 0035

Nowhere. A sweep that cannot stop every thread, or finds a thread it did
not stop, recycles nothing.

### Bugs deliberately not caught

Pointer jumps over a redzone into another live object, by index or by
arithmetic (a later RFC); use after return (a later RFC); a pointer jump
inside one object by arithmetic; uses of a block through a pointer the
program hid (A6); data races between a guard and its access (A3); accesses
to untracked memory (A1); anything in a `WEAVEC_UNSAFE` region or in code
`weavec-cc` did not compile.

### Accepted false traps

- **ASan's own accepted false traps** stay accepted: a scan that reads
  past an object on purpose traps unless the code keeps it from ASan (now
  honoured, §5) or marks it `WEAVEC_UNSAFE`.
- **Code paths taken under ASan.** A program that behaves differently when
  it sees `__SANITIZE_ADDRESS__` behaves as it does under ASan; one that
  calls a sanitizer interface the runtime does not implement fails to link
  (`-fno-weavec-asan-compat` builds it as v0.16.0 did).

### Cost

A sweep stops the program for a time that grows with its live heap and
its writable data; pacing (§3.3) keeps the share of the run time sweeps
take bounded, and the quarantine grows between sweeps instead, up to a
ceiling. A fortified call costs one compare. *Acceptance gates* bound run
time, memory and pauses.

## Detailed design

### 1. The pipeline

As RFC 0035 §1 and its amendment 1 (GuardInsert, GuardPrune, GuardLoop,
GuardExpand), with these additions:

1. **Fortification.** On glibc targets, when the command line neither
   defines nor undefines `_FORTIFY_SOURCE` and optimises, `weavec-cc`
   defines `_FORTIFY_SOURCE=3`. (Darwin's SDK fortifies at level 2 by
   default when optimising.)
2. **ASan's contract** (§5): the unit is parsed with AddressSanitizer
   enabled in its language options, so that
   `__has_feature(address_sanitizer)` is true, `__SANITIZE_ADDRESS__` is
   defined and `no_sanitize("address")` is accepted; before the backend
   runs, AddressSanitizer is disabled again so its instrumentation never
   runs, and GuardInsert removes the `sanitize_address` function attribute.
   Clang predefines `_FORTIFY_SOURCE=0` under ASan on Darwin; the driver
   undoes that. `-fno-weavec-asan-compat` keeps the language options as the
   command line set them.

### 2. Stack frames

RFC 0035 §3 stays (one frame `alloca` with redzones, scopes written from
lifetime markers, dynamic allocas, the unpoisoning before `noreturn`
calls), with one change to §3.2:

**Shared space.** Two tracked static allocas may share bytes of the frame
when their lifetimes never overlap: LLVM's `StackLifetime` analysis (the
one SafeStack uses), in its *may be live* mode, over the lifetime markers
that remain after the optimiser. An alloca without lifetime markers is live
for the whole function and shares with none. The layout places the
allocas in decreasing order of size, each at the lowest 32-byte-aligned
offset after the left zone where its *extent* (the object and its
redzone, as RFC 0035 §3.2 sizes them) overlaps the extent of no alloca
already placed that is live at the same time as it. The frame is the
largest end of an extent, rounded to 32.

Shadow: at entry the frame's shadow is written as before, each granule
taking the value of the alloca live at entry that covers it (addressable
or partial for its object, `0xF2` for its redzone), else `0xF8` when an
alloca's object covers it, else `0xF2`. A lifetime start of a shared alloca
writes its object's granules addressable (as before) and its redzone's
granules `0xF2`; a lifetime end writes `0xF8` over its object, as before.
Two allocas that share bytes are never live together, so the last write
of a granule is always the live alloca's. At `-O0` (no lifetime markers)
nothing shares, as in v0.16.0.

### 3. The heap

#### 3.1 The slot word

Unchanged from RFC 0035 §5.1: `size << 2 | state`.

#### 3.2 Release

`free` and `realloc` validate as before (R); a released slot's shadow
becomes `0xFD` and it joins its class's *quarantine*. It is never reused
from there: only a sweep moves it to the free list. A huge block's mapping
is replaced by an inaccessible reservation at release, as before, and
unmapped only by a sweep that finds no pointer to it.

#### 3.3 When a sweep runs

Let `q` be the quarantine's bytes, `live` the bytes of live heap blocks,
`Q` the configured quarantine (`WEAVEC_RT_QUARANTINE`, 16 MiB by default),
`S = max(Q, live / 4)` the *soft* budget and `H = max(16 Q, live / 2)` the
*hard* budget. A release (or an allocation) sweeps before it returns when

- `q ≥ H`; or
- `q ≥ S`, and the time since the last sweep ended is at least eight times
  that sweep's duration (so sweeping takes at most about a ninth of the
  run time while the program frees faster than sweeps recycle); or
- a class's region is exhausted.

Until the sweep, allocations take fresh slots. The pacing is checked only
when the quarantine has grown by a sixteenth of `S` since the last check.

#### 3.4 The sweep

1. **Stop.** Take the sweep lock and every allocator lock (as `fork`
   does), then stop every other thread: on Darwin with `task_threads` and
   `thread_suspend`, reading each thread's registers with
   `thread_get_state`; on Linux by signalling each task of
   `/proc/self/task` (`SIGPWR`, or `WEAVEC_RT_SWEEP_SIGNAL`), whose handler
   records its stack pointer (the kernel saved its registers on its stack)
   and waits. A thread that does not stop within a second ends the sweep
   without recycling (A7).
2. **Mark.** Scan conservatively, word by aligned word, for addresses of
   quarantined slots and released huge blocks (interior addresses count):
   - each thread's registers and stack from its stack pointer (less the
     ABI's red zone) to the end of the mapping that holds it;
   - every other readable, writable mapping of the process but the
     runtime's own (the shadow, the arena and its metadata, the mark
     bitmaps, the tables) and the stacks; on Darwin, only the pages that
     are anonymous or modified (`mincore`), and of the dyld shared cache
     only the writable segments of the images the process loaded (a table
     the runtime keeps from dyld's add-image callback), not the cache's
     whole writable mapping;
   - the bytes of every live heap block and live huge block.

   Released blocks are not scanned (a pointer in a released block is
   reachable only through a use after free). Marks go to a bitmap per
   class.
3. **Sweep.** Each quarantined slot without a mark moves to its class's
   free list; marked slots stay. Released huge blocks without a mark are
   unmapped.
4. **Resume** the threads and release the locks.

The sweep calls nothing that takes a lock another thread could hold while
stopped (no allocation, no stdio, no dyld). `WEAVEC_RT_STATS=1` adds the
number of sweeps, their total and longest pause, and the blocks and bytes
retained by the last one.

### 4. Globals

RFC 0035 §4 stays. `__weavec_rt_globals_register` and `_unregister` first
find the process's owner (on Darwin), so that a shared image's globals are
poisoned in the owner's shadow; until the owner is resolved, a copy of the
runtime reserves nothing.

### 5. ASan's contract

The runtime defines, with ASan's signatures and meaning:
`__asan_poison_memory_region` (whole granules inside the region become
`0xF7`), `__asan_unpoison_memory_region`, `__asan_address_is_poisoned`,
`__asan_region_is_poisoned`, `__asan_describe_address`,
`__asan_handle_no_return` (clears the stack's shadow, as before a
`noreturn` call), `__asan_get_current_fake_stack` and
`__asan_addr_is_in_fake_stack` (none), `__sanitizer_print_stack_trace`,
`__sanitizer_set_report_path`, `__sanitizer_annotate_contiguous_container`,
`__sanitizer_start_switch_fiber`, `__sanitizer_finish_switch_fiber`, and
LeakSanitizer's `__lsan_ignore_object`, `__lsan_disable`, `__lsan_enable`,
`__lsan_register_root_region`, `__lsan_unregister_root_region`,
`__lsan_do_leak_check` and `__lsan_do_recoverable_leak_check` (no leak
checking: they do nothing). A function marked `no_sanitize("address")`
stays an unsafe region (RFC 0035 amendment 8). A poisoned byte reports
`use-after-poison`.

### 6. Checked library calls

- **Fortified forms.** A call of a `chk` alias whose object-size argument
  is not the constant `(size_t)-1` checks (F): an inline guard checks that
  its byte count is at most the size; a redirected call passes the size to
  its wrapper (every writing wrapper takes a `limit` before its site), which
  checks what it writes against it. A failure within addressable bytes
  reports `intra-object-overflow`.
- **Number parsing.** `strtol`, `strtoll`, `strtoul`, `strtoull`,
  `strtoimax`, `strtoumax`, `strtod`, `strtof`, `strtold`, `atoi`, `atol`,
  `atoll` and `atof` are redirected to wrappers that call the C library and
  then check the bytes it read: up to the end pointer and the character
  that stopped it, or, when nothing was converted, the leading white space
  and sign and the character after them (ASan's rule).
- **The library table** keeps only what the guards read (access, bytes,
  count, str, null, release and realloc, wrapper, wide, variadic, format,
  disjoint, value, chk); the analysis's clauses go with the analysis.

### 7. Modes and options

RFC 0035 §7 stays but:

- Deleted, with no aliases: `-fweavec-diagnose`, `-fweavec-budget=`,
  `-fweavec-unit-budget=`, `-fweavec-dump-analysis`,
  `-fweavec-analysis-stats=`, and the `-W…weavec…` flags (they reach Clang
  as unknown warning options).
- New: `-fno-weavec-asan-compat` (§1).
- Zero-initialisation keeps its 4 KiB limit: a stale pointer in an
  uninitialised local is a pointer the sweep sees, so it cannot reach a
  reused block (A6).

### 8. Deletions

- **Tools and libraries**: `tools/weavec`; `lib/Analysis` and
  `include/weavec/Analysis` (`Annotations.h`'s `WEAVEC_UNSAFE` spelling
  moves to `UnsafeRegions`); `lib/Core` and `include/weavec/Core` but the
  library table (`LibrarySpec`), trimmed to what the guards read;
  `lib/Frontend`'s `FrontendAction`, `ProgramAnalysis`, `ProgramChecks`,
  `InterfaceFacts`, `AnalysisSummary`, `AnalysisStats`,
  `ClangDiagnosticSink` and `DiagnosticControl`; the driver's analysis
  flags.
- **The header**: `weavec.h` keeps `WEAVEC_UNSAFE` (and its version
  macros); every other annotation is deleted.
- **Tests**: `test/Analysis`, `test/WholeProgram`, the analysis parts of
  `test/Annotations`, `unittests/Analysis`, the `unittests/Core` tests but
  `LibrarySpecTest`, the `unittests/Frontend` tests of deleted files; the
  case runner's analysis (`BUG`, `MISS` on a `BUG`, `NEUTRALISED`, `ALLOW`,
  `TOOL`); the case suites that pin only the analysis; the corpus gate's
  analysis findings, triage entries and whole-program configs; the
  documentation examples of `weavec`.
- **Documentation**: `docs/annotations.md`'s annotations but
  `WEAVEC_UNSAFE`, `docs/data/diagnostic-remedies.json`, the diagnostic
  reference, the guides of ownership, whole-program analysis and
  annotations, and the internals pages of the analysis.
- **Runtime**: the byte-budget recycling of the quarantine.

The RFCs are kept; RFCs 0001–0017 and 0031 are marked Superseded by this
RFC, and RFC 0030 keeps Implemented with its analysis superseded.

### 9. Tests

- **Cases.** `test/cases/determinism/` holds a case per row of the
  motivation's table, each with a correct twin: a heap-spray use after
  free, a use after free and a double free after churn, the field
  `strcpy`, a pool poisoned through ASan's interface, deep recursion
  through escaping locals in disjoint scopes (no stop at the reference's
  depth), and the three bugs this RFC leaves to a later one as expected
  misses (`MISS`). `BUG` lines of the remaining suites become `TRAP` where
  the default build stops on them; analysis-only cases are deleted.
- **Lit.** `test/Guards/` pins the IR of the shared frame layout and each
  new report line; `test/Driver/` the fortification define, ASan's macros,
  and a dylib's and a bundle's global overflow.
- **Runtime test.** `rt_test.c` covers the sweep (single and
  multi-threaded, a retained block, a recycled one, pacing), the image
  table, the ASan interface and the number parsers.

## Annotation surface

`weavec.h` keeps `WEAVEC_UNSAFE` (function and block forms) and its
version macros, and moves to version 1.0; every other macro is deleted
(`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`,
`WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_COUNTED_BY`,
`WEAVEC_ENDED_BY`, `WEAVEC_STRING`, `WEAVEC_ASSUME`, the reference-counting
forms and the rest). A program that uses one fails to compile with
`weavec-cc` and other compilers alike until it stops; the macros expanded
to nothing elsewhere.

## Diagnostics

Every `weavec::core::diag` id is deleted with the analysis. New run-time
report kinds (not diagnostics; RFC 0035 §5.3): `intra-object-overflow`,
`use-after-poison`.

## Implementation plan

The stages land on `rfc0036-deterministic-safety`, on top of the first
draft's implementation, and ship as one change:

- **S0 The RFC**: this revision.
- **S1 Remove the first draft's spatial and frame mechanisms**:
  provenance bounds, escape checks and the escape registry, pointer slots,
  frame blocks, frame regions and capture logs, the `setjmp` hook, the
  globals table and `__weavec_rt_bounds`; RFC 0035's native frames back.
- **S2 Shared frame space** (§2).
- **S3 The sweep's cost**: pacing (§3.3) and the image table (§3.4).
- **S4 Cases, lit and runtime tests** for the revised scope.
- **S5 Fresh iterations**: the motivation's projects, the corpus, the
  gates, the documentation; Linux through CI.

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference is
`$WEAVEC_LLVM_PREFIX/bin/clang`; v0.16.0 is built from its tag on the same
machine; darwin-arm64 unless named; ratios of best-of-three user CPU on an
idle machine.

**Detection**

- **G1. Determinism.** Every `_bug` case of `test/cases/determinism` that
  this RFC catches stops with its report kind and its twin runs clean, in
  trap and verify mode, on darwin-arm64 and linux-x86_64.
- **G2. No loss.** The detection, detection-blind and soundness suites stop
  at least as many bugs as v0.16.0, with no twin stopping.
- **G3. Monitors.** No `weavec.proven` report in verify mode on any case.

**Drop-in**

- **G4. Untuned programs.** CPython 3.13.7 (`test_ast`, `test_functools`
  and the motivation's subset), SQLite 3.50.4 (`veryquick`) and coreutils
  9.5 (`make check`) build with `CC=weavec-cc` and pass as the reference
  does, but for tests that misuse memory on purpose and that the project
  skips under ASan; CPython's extension modules' globals are protected.
- **G5. No regression.** The corpus configs build and pass their suites
  (`scripts/corpus-gate.py --full`).

**Cost**

- **G6. Run.** The corpus benchmarks' geometric mean at most 1.10×
  v0.16.0's, and no benchmark above 1.25× its v0.16.0 ratio.
- **G7. Memory.** Peak RSS of each benchmark at most 1.5× v0.16.0's or 64
  MiB above it, whichever is larger.
- **G8. Pauses.** On a benchmark with 256 MiB of live heap, the longest
  sweep at most 150 ms.
- **G9. Build.** No project's build more than 1.1× v0.16.0's.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes; the library (C++ under
  `lib/`) is at most 9,000 lines.
- **H2.** The documentation describes the change (README, guarantees, CLI,
  architecture, roadmap, AGENTS.md); `npm test && npm run build` in
  `docs/`; this RFC is marked Implemented.

## Implementation amendments

These record where the implementation departs from the design above. They
override the body.

1. **The budgets (§3.3).** `q` is the bytes the quarantine gained since the
   last sweep: the blocks the last sweep retained (a stale word still points
   to them) are not counted, or a program that keeps dangling pointers would
   sweep on every release. The hard budget is `max(2 Q, live / 2)`, not
   `max(16 Q, live / 2)`: with the larger floor pacing let libgit2's
   quarantine reach 116 MiB beside 86 MiB of live heap, and its peak memory
   1.8 times v0.16.0's; with the smaller one 82 MiB, 1.5 times, for 3% more
   time. `WEAVEC_RT_STATS=1` prints the peaks of both.
2. **The shared layout (§2).** The allocas are placed in RFC 0035's order
   (larger alignments first, then source order), not by decreasing size,
   so that a frame whose locals all live throughout is laid out as before.
   A lifetime start writes its local's redzone again only when that redzone
   overlaps another local's object; elsewhere the redzone is never
   written over. The shadow of a granule two locals share is `0xF8` at
   entry.
3. **The roots on Darwin (§3.4).** Walking the dyld shared cache's writable
   mappings page by page cost 12–25 ms per sweep whatever the heap (cJSON:
   93 sweeps, 1.1 s of 3.2 s), and so did two mappings outside it: the
   kernel's reclaim ring of the system allocator (64 MiB, tag
   `VM_MEMORY_VM_RECLAIM`, never a program pointer) and the stack below its
   stack pointer. The sweep now asks each mapping for its counts of dirty
   and swapped-out pages (one call per mapping) and skips a mapping with
   none and the reclaim ring; scans a stack from its stack pointer without
   asking which of its pages are resident; and scans the writable segments
   of the cached images the process loaded, from dyld's add-image and
   remove-image callbacks, instead of the cache's mappings (3.4 MB of
   segments against about 200 MB). A sweep with a small heap takes about a
   millisecond.
4. **Reuse in address order (§3.4).** A sweep rebuilds each class's free
   list and quarantine by walking its slots from the last, so that slots
   are reused, and their words read, in address order. Reuse in the
   quarantine's order after a sweep scattered allocations over the region
   (cJSON's allocation path missed the cache).
5. **Released large slots (§3.2).** A released slot of at least 64 KiB
   gives its whole pages back at once: on Darwin by mapping fresh pages over
   them, since `MADV_FREE` leaves them resident until the system needs
   memory, and the slot now waits for a sweep while the program takes fresh
   ones. Without it brotli's peak memory was 2.2 times v0.16.0's.
6. **Small shadow writes (§3).** An allocation and a release write a small
   object's few shadow bytes with stores rather than a call of `memset`.
7. **The sweep tests.** A conservative scan keeps a block that a stale copy
   of its address in an unused stack slot points to. The runtime test's
   sweep cases scrub the stack below them with a compiler barrier (the
   optimiser removed the stores to a dead array, and with them the scrub),
   and use size classes no other test touches.
8. **Shared space only in large frames (§2; *Where this is less sound
   than RFC 0035*).** Sharing has a cost the body missed: a use after scope
   that lands in bytes a later local now owns passes. In verify mode, where
   every local a marker names is tracked, a detection case's temporary
   (`int tmp[4]`) shared its bytes with the next block's `int other[16]`,
   and the read through the stale pointer passed (ASan keeps such locals
   apart for the same reason). Locals now share bytes only in a frame that
   would be larger than 512 bytes without sharing: below that the redzones
   cost little, and every local keeps its own bytes as in RFC 0035. So RFC
   0036 is less sound than RFC 0035 in one place: in such a large frame, a
   use after scope that reaches a later local's bytes passes.
9. **Fortified sizes on glibc (§1, §6, (F)).** glibc fortifies through
   inline wrapper functions (`strcpy` calls `__builtin___strcpy_chk(d, s,
   __builtin_dynamic_object_size(d, 1))` inside itself). Clang cannot see a
   field from inside the wrapper, so after inlining the size is the rest of
   the object (12 bytes for an 8-byte `name` followed by an `int`), not the
   field. (F) holds for the size the call states, so on glibc a write from a
   field into the next stays inside it and passes, as it does with plain
   Clang there; Darwin's headers fortify with macros at the call site and
   pass the field's size. The tests of (F) spell the call out as Darwin's
   headers produce it (`__builtin___strcpy_chk` with
   `__builtin_object_size(field, 1)`), so they test the check on every
   platform. Defining `_FORTIFY_SOURCE=3` on glibc also makes glibc's own
   fortified wrappers replace a header's inline copies of them (a case that
   models glibc's mechanism with its own wrapper builds with
   `-U_FORTIFY_SOURCE`). Field sizes the guards compute themselves, from
   the address's `getelementptr`, would close the gap; that is future work
   for an RFC.

10. **Released huge blocks and the roots (§3.2–§3.4).** A released huge
    block holds no memory, but its reservation holds address space and, on
    Linux, a mapping of the process's limited count until a sweep unmaps
    it; the body counted only quarantined slots in `q`, so a program that
    freed only huge blocks never swept (128 releases of 600 MiB blocks kept
    75 GiB reserved). A released huge block now counts toward `q` at its
    size until a sweep unmaps it, and toward what a sweep retained while a
    word still points to it. The mark is also stated more exactly: on
    Linux, a mapping of a file under `/dev` is device memory and is not
    read, but for shared memory (`/dev/shm`, shared anonymous mappings,
    which `/proc/self/maps` names `/dev/zero`, and hugetlbfs under
    `/dev/hugepages`); a sweep that cannot read every root (on Linux,
    `/proc/self/maps`; on Darwin, the walk of the address space ends early)
    recycles nothing, as one that cannot stop every thread does (A7).

## Measured gates

Measured 2026-10-09 on the branch (darwin-arm64, Apple M3, `release`
preset, reference `clang` 23.1.0; v0.16.0 built from its tag on the same
machine). The RFC stays **Accepted**: linux-x86_64 is not measured (the
local Docker daemon does not answer; CI on the pushed branch measures it),
and G6 holds for every benchmark but QuickJS, whose excess is the cost of
the ASan contract (below).

| Gate | Result |
| --- | --- |
| G1 Determinism | **Pass on darwin-arm64**: the seven cases of `test/cases/determinism` this RFC catches stop with their kinds and every twin runs clean, in trap and verify mode; the four left to a later RFC are `XFAIL`. linux-x86_64 **not measured**. |
| G2 No loss | **Pass**: the detection and detection-blind suites stop 105 of 109 bugs (v0.16.0: 104; the first draft: 106), and the eight detection cases of `semantics/` all stop; no twin stops; every case passes but 11 `XFAIL` (533 pass). |
| G3 Monitors | **Pass**: no `weavec.proven` report on any case in verify mode. |
| G4 Untuned programs | **Pass**, with one finding shared with ASan. CPython 3.13.7: `pyconfig.h` identical to the reference's; no warning at build or run (v0.16.0 printed 70 for extension modules); `test_ast`, `test_functools`, `test_regrtest` and the 21-file subset pass (6,364 tests); the full suite fails, run file by file, the same files as the reference build on this machine but `test_faulthandler`, whose `test_read_null` reads null on purpose and gets WeaveC's report. SQLite 3.50.4 `veryquick`: 24 errors in 329,057 tests, the same five files as the reference (Tcl 8.5); `capi3c` passes (SQLite skips its deliberate misuse under ASan). coreutils 9.5: `config.h` identical; `make check` 471 pass against the reference's 475: `tr` passes (the number parsers), and `help-version`, `csplit`, `shuf` and `tty-eof` stop at gnulib's word-at-a-time `rawmemchr` over-read, which ASan stops too (verified). |
| G5 No regression | **Pass**: every corpus config builds and passes its suite in trap and report mode, with no trap (`scripts/corpus-gate.py --full`); the injections stop 33 of 39, the same six misses as v0.16.0; jansson's four triaged guard failures are gone (it takes its exact path under ASan's macros). |
| G6 Run | **Geometric mean passes**: 1.05 times v0.16.0's (1.61 against 1.54 the reference; best of two runs of three). Every benchmark is within 1.25 times its v0.16.0 ratio but QuickJS, at 1.31 (2.37 against 1.80): under `__SANITIZE_ADDRESS__` QuickJS sends every object to `malloc` (8.3 million allocations instead of 76,000), so its objects are tracked one by one. Built the same way (`-D__SANITIZE_ADDRESS__` for v0.16.0), v0.16.0 takes 3.2 s and this tree 2.72 s; with `-fno-weavec-asan-compat` it is 1.06 times v0.16.0's. The others: cJSON 1.04, zlib 0.97, Lua 0.97, LMDB 0.95, Janet 1.05, brotli 1.13, xz 0.95, libdeflate 1.00, zlib-ng 0.99, curl 0.96, cmark 1.18, libgit2 1.17 (the hard budget of amendment 1 cost cmark, libgit2 and brotli 5–10 points). |
| G7 Memory | **Pass** on every benchmark (peak RSS, MiB, against v0.16.0's): cJSON 89 (71), zlib 3 (3), Lua 144 (176), QuickJS 202 (142; limit 214), LMDB 214 (210), Janet 59 (47), brotli 162 (441), xz 98 (98), libdeflate 105 (105), zlib-ng 3 (3), curl 32 (31), cmark 365 (473), libgit2 170 (117; limit 181). `scripts/corpus-gate.py --bench` now records each build's peak RSS. |
| G8 Pauses | **Pass**: 33 ms longest sweep with 256 MiB of live heap (7 sweeps). |
| G9 Build | **Pass**: compile CPU of the corpus gate's units 0.86 times v0.16.0's in total; no project above 1.07 times (curl), the worst unit 1.42 times (llhttp.c, 0.46 s). |
| H1 | **Pass** (`scripts/check-hygiene.py`; library 7,200 lines, runtime 5,000). |
| H2 | **Pass** (README, guarantees, CLI, compatibility, troubleshooting, architecture, roadmap, AGENTS.md; `npm test`, `npm run build`). |

## Drawbacks

- **WeaveC no longer reports bugs before the program runs.** Teams that
  want static findings use the Clang static analyzer or another tool.
- **Pauses.** A sweep stops every thread; a large live heap pays for each
  one. Concurrent marking is future work.
- **Memory.** Between sweeps the quarantine may grow to the hard budget
  (§3.3); a block some stale word points to stays allocated until the word
  changes.
- **A signal on Linux.** The sweep uses one signal; a program that uses
  `SIGPWR` itself sets `WEAVEC_RT_SWEEP_SIGNAL`.
- **The ASan contract** can take code paths written for ASan, with their
  cost (QuickJS sends every object to `malloc`), and can call sanitizer
  interfaces the runtime does not implement (a link error, which
  `-fno-weavec-asan-compat` avoids).

## Alternatives

### The first draft: provenance bounds and frame blocks

Measured 2026-10-08 on the first draft's tree with each mechanism switched
off in turn (the corpus's 13 benchmarks, ratio to the reference; the
switched-off frames are native frames without redzones):

| | v0.16.0 | none of them | frame blocks only | bounds only | first draft |
| --- | --- | --- | --- | --- | --- |
| cJSON | 1.89 | 2.49 | 2.66 | 2.67 | 2.81 |
| zlib | 1.26 | 1.27 | 1.28 | 1.83 | 1.83 |
| Lua | 2.25 | 2.36 | 2.89 | 4.03 | 5.00 |
| QuickJS | 1.78 | 3.01 | 8.89 | 5.68 | 10.79 |
| LMDB | 1.23 | 1.28 | 1.87 | 2.23 | 2.71 |
| Janet | 1.36 | 1.41 | 2.33 | 2.26 | 3.27 |
| brotli | 1.89 | 2.33 | 2.34 | 3.30 | 3.37 |
| xz | 1.30 | 1.28 | 1.33 | 2.10 | 2.06 |
| libdeflate | 1.32 | 1.32 | 1.33 | 1.56 | 1.56 |
| zlib-ng | 1.36 | 1.36 | 1.38 | 1.90 | 1.91 |
| curl | 1.82 | 1.79 | 3.02 | 2.50 | 3.56 |
| cmark | 1.69 | 2.55 | 3.93 | 3.62 | 4.84 |
| libgit2 | 1.19 | 2.11 | 2.23 | 2.63 | 2.56 |
| **geometric mean** | **1.53** | **1.80** | **2.32** | **2.62** | **3.08** |

Bounds and escape checks cost about 46% on their own (a lookup of a root's
object is about 25 instructions and six loads, and an interpreter's loop
has many roots no loop hoists; Lua's `luaV_execute` grew from 19,600 to
64,000 instructions), frame blocks about 29% (a runtime call at entry and
exit of every function with a local a guard reaches, and QuickJS's frames
kept by pointers in their callers' frames wait for sweeps). The rest (1.53
to 1.80) was two costs this revision removes: each sweep's fixed cost of
12–25 ms, most of it `mincore` over the dyld shared cache's whole writable
mapping (cJSON: 93 sweeps, 1.1 s of its 3.2 s; with the quarantine at
1 GiB it matched v0.16.0), and sweeps that were not paced against the run
time. The branch had tried cheaper bounds (run-time span checks: no gain;
slot-granular bounds without the slot word: 10% on Lua) before this
measurement. Meeting the first draft's cost gate (1.25× v0.16.0) would
need about two thirds of both costs gone, with no design in hand that
does it; a later RFC starts from that budget.

### Others

- **Ownership contracts at function entry** (the roadmap's RFC 0036):
  removes guards on the strength of inferred facts, which would have to be
  right where the analysis measured 62 false findings in 69.
- **A larger quarantine** (ASan's 256 MiB): makes the use after free of the
  motivation pass later instead of never.
- **Fat pointers** (CHERI's model, SoftBound's): exact bounds that survive
  integer round trips, at the price of the ABI.
- **Tags** (HWASan, MTE): probabilistic for far jumps without hardware,
  and Darwin user space has neither top-byte tags nor MTE on the reference
  machine.
- **ASan's macros off by default**: QuickJS then keeps its own small-block
  allocator, whose objects the guards cannot tell apart; this RFC prefers
  the detection and measures its cost (G6).

## Prior art

- **MarkUs** (Ainsworth and Jones, S&P 2020) and **MineSweeper** (Erdős et
  al., ASPLOS 2022): quarantine released memory until a conservative mark
  finds no pointer to it.
- **AddressSanitizer**: the shadow, the poisoning interface, stack slot
  reuse by lifetime (`-fsanitize-address-use-after-scope` with Clang's
  stack colouring).
- **`_FORTIFY_SOURCE`**: subobject sizes for string functions.
- **Boehm–Demers–Weiser GC**: stopping threads by signal, conservative
  root scanning, finding roots in loaded images' data segments.
- **SafeStack**: `StackLifetime`-based sharing of a frame's objects.

## Unresolved questions

- **The quarantine's budgets and the pacing factor** (`live / 4`,
  `live / 2`, eight times the last sweep) trade memory for time; the gates
  measure them.

## Future work

- Spatial determinism (an access checked against its pointer's object)
  and use after return, under a cost budget measured before the design:
  low-fat pointers with power-of-two classes, bounds only at escapes, or
  hardware tags.
- Concurrent marking, so that a sweep does not stop the program for its
  whole duration.
- Hardware memory tagging (Arm MTE, Apple MIE) where the processor has it.
- Adoption: relocatable installs, packages, a vendored `weavec.h`.
