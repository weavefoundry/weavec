# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

The design today is [RFC 0054](rfcs/0054-one-of-several-types.md) for values of one of several types (tagged unions, checked conversions and downcasts), [RFC 0053](rfcs/0053-callables-and-containers-by-their-loans.md) for callables and containers by their loans and [RFC 0052](rfcs/0052-what-a-call-may-reach.md) for what a call may reach in safe C++ (reach by types, results, views, callables, values built from borrows), [RFC 0051](rfcs/0051-nothing-undefined.md) for what a safe function may not leave undefined (the list of the two languages' undefined behavior, the definitions and the stops, the rules that reject what asserts, the comparison of safe units at the link), [RFC 0050](rfcs/0050-checked-once.md) for what a fast safe unit checks once (views, the typed allocator's paths, Safe C's library calls), [RFC 0049](rfcs/0049-errors-that-carry-their-fix.md) for the fixes errors carry and `--weavec-fix`, [RFC 0048](rfcs/0048-safe-code-without-the-shadow.md) for what a fast safe unit checks and believes, [RFC 0047](rfcs/0047-conversion-in-place.md) for converting existing C++, [RFC 0046](rfcs/0046-thread-safety.md), [RFC 0045](rfcs/0045-type-stable-memory.md) and [RFC 0044](rfcs/0044-safe-cxx.md) for safe C++, [RFC 0043](rfcs/0043-ownership.md), [RFC 0042](rfcs/0042-typed-memory.md) and [RFC 0041](rfcs/0041-safe-c.md) for safe C, and [RFC 0038](rfcs/0038-drop-in-everywhere.md), over the
sweep and colors of [RFC 0037](rfcs/0037-complete-sweep-and-colors.md), the
reuse after a sweep of [RFC 0036](rfcs/0036-deterministic-heap-reuse.md)
and the guards of [RFC 0035](rfcs/0035-guard-by-default.md). The
milestones before it are summarized under _History_; their RFCs keep the
detail, and the per-RFC validation records of the milestones before RFC
0030 remain in the repository history at tag `v0.10.0`.

## Now: RFC 0054 — One of several types

Design: [RFC 0054 — One of several types](rfcs/0054-one-of-several-types.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_ say
how). A third of the rule errors of C as written had no fix at all: a
union that holds a pointer, and a `void *` or a base converted back to what
it points to. Each is now a checked form: a union says its tag
(`WEAVEC_TAGGED_BY`, `WEAVEC_WHEN`) and its members are used under a
comparison the compiler adds; a conversion asks the runtime, which records
the type of every block, global and recorded local, whether an object of
the target type is there; a C++ downcast compares the type information its
vtable names. `bad-cast` and `union-member` stop the program in both builds.
Runtime ABI 49, ledger 14, audit 10, `weavec.h` 3.5.

- [x] **The RFC**, accepted as drafted on 2026-11-22.
- [x] **S0 to S5**: measured first (checked casts 1.009, member checks
      1.006, a conversion 4.3 ns), then the records, the conversions, the
      downcasts, the tagged unions, E9, the ledger, the audit and the pages.
- [x] **The loop** (G3): the trial interpreter made safe in 6 minutes with
      its casts kept, at 1.62 times the program as written (asks 1.70).
- [x] **A conversion** (G5): nng from 60% to 86% of its functions safe.
- [x] **Cost** (G7, G8): within 1.01 of main's fast build (the LRU port
      1.019, 1 ms), 1.02 of its checked one; compiles 1.006.
- [ ] **The survey** (G2): 583 of 1,014 `pointer-cast` errors gone (asks
      600); `--weavec-fix` clears 402 of 1,853 `union-pointer` errors (asks
      half: mujs's tag overlaps its union, Lua tests its tag by bits).
- [ ] **New C** (G4): a json port in Safe C in 7.5 minutes, but at 3.0 times
      Rust (asks 1.5; Clang's build of it is 1.55).
- [ ] **A round** (G6): 33 holes found and 29 closed; 4 left, each memory
      with no record of its type.

## Before: RFC 0053 — Callables and containers by their loans

Design: [RFC 0053 — Callables and containers by their loans](rfcs/0053-callables-and-containers-by-their-loans.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_ say
how). RFC 0052's fresh round found 71 holes, two thirds through callables
the rules followed only when they could find the lambda. RFC 0053 makes
every callable a view: its call, or a call it is passed to, ends the
borrows into what its loans reach, narrowed by a body the function wrote;
the library's containers of views are views however they are made; a
polymorphic object carries the loans it was made from; code of the
program may change any mutable global; `mutable` makes `const` shallow for
a caller; and a value's type, or `WEAVEC_LIFETIMEBOUND`, says what it
borrows.

- [x] **The RFC**, measured first (S0), accepted on 2026-11-22.
- [x] **RFC 0052's round** (G1): all 90 programs rejected.
- [x] **Correct code** (G2): 10 new errors in 10,413 safe functions.
- [x] **The loop** (G3): interp in 4 minutes, kv in 12.
- [ ] **A fresh round** (G4): 74 holes; 62 rejected since, 12 left
      (polymorphic objects inside library containers, reference wrappers
      of views, `std::tie`, an owner's destructor, a library polymorphic
      base, types nested 65 deep, a span of reference wrappers).

## Before: RFC 0052 — What a call may reach

Design: [RFC 0052 — What a call may reach](rfcs/0052-what-a-call-may-reach.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_ say
how). A probe round found 24 safe C++ programs that misuse a borrow, each
through a way to change an object the borrow rules did not list. RFC 0052
makes the default deny: what a call may reach is read from types; a
reference or pointer a call returns points into every argument whose type
can hold it; `const` on a view is shallow; a lambda the function sees ends
what its body changes where it is called and passed; a callable passed
beside what it changes is `call-alias`; a kept callable captures no
reference, `this` or pointer; and a value built from a borrow is a view of
it.

- [x] **The RFC**, accepted as drafted on 2026-11-21.
- [x] **S0 to S5**: measured first (20 new errors in 10,871 safe
      functions, under D8's 2 per thousand), then reach and results,
      views, callables, values built from borrows, the fixes (E8), the
      pages.
- [x] **The round's holes** (G1): all 24 lifetimes holes and the types
      area's 9 are rejected.
- [ ] **A fresh round** (G4): 71 more holes of the same classes, through
      callables the flow does not read (`std::bind`, a lambda in a `pair`
      or behind a pointer, a functor), library containers of views, and a
      view behind a base. RFC 0053 closes them.
- [ ] **The loop** (G3): interp in 10.5 minutes (asks 9); kv in 13 (asks
      19.5).

## Before: RFC 0051 — Nothing undefined

Design: [RFC 0051 — Nothing undefined](rfcs/0051-nothing-undefined.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_ say
how). Ten RFCs gave safe C and C++ rules an agent iterates on; none was
about what else a compiler assumes. RFC 0051 makes a safe function have no
behavior the language leaves undefined: signed arithmetic wraps, shifts
and float conversions have one result, loops may run forever, every local
is zeroed, struct copies are moves; unreachable code, a missing return, a
division by zero or of the least value by -1 and a full stack stop with a
report; `[[assume]]`, `restrict`, asserting attributes, bytes into a
`bool` and signal handlers registered by safe code are rejected; five of
Clang's warnings are errors; and safe units that disagree on a type, a
function or a global do not link. [The list](../resources/safe/undefined.md) gives
each of C23's 221 items and C++'s 80 their disposition and test. Runtime
ABI 48, ledger 13, audit 9.

- [x] **The RFC**, accepted as drafted on 2026-11-20.
- [x] **S0 to S6**: the list, the definitions, the stops and the runtime,
      the rules, the comparison at the link, the ledger and the audit, the
      pages.
- [x] **The gates**: every probe stops or is rejected in both builds;
      every program within 1.02 of main's fast build (the key-value
      server's noisy benchmark 1.08 and 0.95); compiles within 1.02.
- [ ] **The comparison at the link**: 4 ms, 4.3% of libzmq's link (asks
      2%); reading the objects is all of it.
- [ ] **The soundness probes** (2026-11-21): about 290 adversarial safe
      programs found about 80 that compile without an error and corrupt
      memory, race or reach undefined behavior unstopped, from about 30
      causes. Five are fixed: a fresh block's bytes, bytes written into a
      pointer, local types sharing a pool, writes into a field in a safe
      function, and `realpath`'s and `gcvt`'s buffers (RFC 0051 amendment
      18, RFC 0045 amendment 19, RFC 0037 amendment 16). Most of the rest
      are borrows that a call through a closure or a reference ends, and
      references a function returns into its arguments, which a design
      change of their own will take on; then threads and the link's
      records.

## Before: RFC 0050 — Checked once

Design: [RFC 0050 — Checked once](rfcs/0050-checked-once.md) (Accepted,
built; its _Implementation amendments_ and _Measured gates_ say how). For
new code the loop is no longer behind Rust's, and the result is: an
agent's all-safe `json` port ran at 1.55 times Clang. RFC 0050 checks each
fact once in a fast unit: the library's `std::string_view` and `std::span`
are trusted as its owners are (a view made from a pointer is guarded once
where it is made), a range-based `for` reads its elements unchecked, a
pooled slot safe C++ released keeps its shadow and the typed allocator
pops and pushes on a short path, and Safe C rejects a library call on a
pointer to one object when it compiles. Runtime ABI 46.

- [x] **The RFC**, accepted as drafted on 2026-11-18.
- [x] **S0 to S5**: measured first (the `json` port at 1.22 with both
      parts, under (D5)'s 1.25), then the views, the range-based `for`,
      the allocator's paths, Safe C's calls, the guarantees and the guides.
- [ ] **The cost gates (D1)**: the `json` port 1.22 (asks 1.15), the
      conversions 1.43 and 1.52 (ask 1.25 and 1.30), Safe C unchanged, the
      allocator 1.10 times Darwin's and 1.73 times glibc's (asks 1.0). The
      RFC is not Implemented until they pass or the owner changes them.

## Before: RFC 0049 — Errors that carry their fix

Design: [RFC 0049 — Errors that carry their fix](rfcs/0049-errors-that-carry-their-fix.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_
say how). A conversion in place took an agent as long as a rewrite into
Rust, and most of what it wrote said what the code already does: a bound
a `memcmp` implies, a parameter a constructor stores. RFC 0049 makes every
rule error data with the fixes the compiler can write from the uses it
reads inside one function (certain, likely, or a choice it prints and
never writes), says the text in each note as a fix-it, and adds
`weavec-cc --weavec-fix`, which applies them over a project from a
survey's ledgers, runs the rules again, takes out a likely fix that
brought an error with no fix, repeats, and marks the files left with no
error safe. Ledger 12, audit 8.

- [x] **The RFC**, accepted as drafted on 2026-11-17.
- [x] **S1 to S6**: errors as data, the certain fixes and the mark, the
      evidence and the likely bounds, a constructor's initializer checked
      as an assignment is, `WEAVEC_LIFETIMEBOUND` and members that borrow,
      the fixer, the audit's plan, the `fix` page and the guides.
- [x] **The gates** on darwin-arm64 and the Linux container: the fixer's
      annotations agree with the earlier conversions' every time (G3); it
      clears 17% of the five projects' errors (G2 asks half), and two fresh
      conversions took 1,015 s and 1,341 s (G4 asks 570 and 580).
- [ ] **Next**: fixes that rewrite (owners, `const` twins, in-place
      strings), the errors the fixer leaves.

## Now: RFC 0048 — Safe code without the shadow

Design: [RFC 0048 — Safe code without the shadow](rfcs/0048-safe-code-without-the-shadow.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_
say how). The loop was at parity with Rust and the result was not: the
tinyxml2 conversion, 94% safe, ran 1.7 times Clang, 6% faster than with no
safe function, because safe code was checked against the shadow as if the
rules had said nothing; and a pointer member could reach a frame that had
returned. RFC 0048 checks a fast unit's safe code against what it holds
(types, bounds, terminators), reads the shadow only where pointers arrive
from code that is not safe, requires `WEAVEC_ESCAPES` where a function
keeps a parameter's address, places a kept local in typed memory, makes
ends `WEAVEC_COUNTED_BY(0)`, and checks a build without optimization by
default.

- [x] **The RFC**, accepted as drafted on 2026-11-15.
- [x] **S0 to S8**: belief by type tags and field annotations, string
      steps checked where made, constant-time heap strings, library calls
      in hand, kept parameters and placed locals, plain frames, the
      `call-args` and `index-single` cases for positions and ends, the
      pools' larger slot lists, the `-O0` default, ledger 11 and audit 7,
      the rules' pages and guides; tinyxml2's conversion brought back to a
      clean compile.
- [ ] **Next**: the gates not met (see the RFC's _Measured gates_); views'
      trusted members and range-based `for` (§6.1, §6.2), not built; the
      allocator's fast path.

## Now: RFC 0047 — Conversion in place

Design: [RFC 0047 — Conversion in place](rfcs/0047-conversion-in-place.md)
(Accepted, built; its _Implementation amendments_ and _Measured gates_
say how). Every measurement of the agent loop was a port of a
specification; RFC 0044's conversion of existing C++ was never run, and
run now it failed before an agent started: 93% of the errors on four
corpus projects came from rules that ban raw pointers. RFC 0047 gives
Safe C++ Safe C's bounds on raw pointers, `WEAVEC_ALIASED` classes that
may be changed through pointers kept in memory and `std::shared_ptr`
(closing a hole through the latter), `WEAVEC_SAFE_FILE`, and a survey
whose audit prints the conversion's plan.

- [x] **The RFC**, accepted as drafted on 2026-11-15.
- [x] **S1 to S7**: bounds in the checker and the passes, the table's
      `counts`, `counted`, `ended` and `strings` clauses, byte views and
      `new` into a smart pointer, aliased classes (rules W, K, S), safe
      files, the survey, the ledger (version 10), the audit (version 6),
      the rules' pages and the guide _Convert C++ in place_.
- [x] **S8 on darwin-arm64**: tinyxml2 and yaml-cpp converted in place by
      agents (94% and 99% of functions safe, their tests passing, public
      headers unchanged); a real out-of-bounds read found in yaml-cpp;
      RFC 0044's and RFC 0046's sets as before but rule W's four.
- [ ] **Next**: the gates not met: a conversion no faster than a Rust
      rewrite of tinyxml2 (G4), 1.7 and 2.1 times Clang (G5), `call-args`
      on library calls (G8); headers whose templates fail in other units;
      the forty spatial bugs (S0).

## Now: RFC 0046 — Thread safety

Design: [RFC 0046 — Thread safety](rfcs/0046-thread-safety.md) (Accepted,
built; its _Implementation amendments_ and _Measured gates_ say how). Five
RFCs gave an agent Rust's loop for memory errors and assumed threads away;
a safe C++ program with a data race compiled, and in the fast build five of
twelve seeded races ran clean. RFC 0046 makes Safe C++ free of data races
by rules alone: kinds _sendable_, _shareable_ and _frozen_; what a thread
start gives a thread is checked; a local borrowed by a thread is lent to
it; a class's one mutex guards its other members; every global is
shareable, `const` or `thread_local`. Nothing at run time changes. Safe C
gets none of it (a safe C function may not start a thread).

- [x] **The RFC**, accepted as drafted on 2026-11-14.
- [x] **S0**: sixty seeded races with fixed twins and forty correct
      programs, written by an agent that had not seen the RFC and confirmed
      under ThreadSanitizer.
- [x] **S1 to S6**: the table's clauses, the kinds, thread starts and
      globals, loans and thread lifetime, locks, thread members,
      `WEAVEC_SENDS`, the `const` path, the ledger (version 9), the audit
      (version 5), the rules' pages and the guide's "Threads".
- [x] **S7 on darwin-arm64**: all fifteen probes and all sixty seeded
      races stop the compile; 32 of 40 correct programs compile unchanged;
      28 of 60 fixed twins are rejected (gate G3 allows 12; the causes are
      listed in the RFC); objects identical with the rules off; compile
      time at most 1.03 times.
- [ ] **Next**: the fixed twins' causes (a lock beside its data outside a
      class, pools that wait for idle), the gates in the Linux container,
      a run-time backstop for the rules in the checked build.

## Now: RFC 0043 — Ownership

Design: [RFC 0043 — Ownership](rfcs/0043-ownership.md) (Accepted, built).
Safe C checked where a pointer may point and what it may be converted to,
and said nothing when a function read a block it released, released it
twice, never released it, or kept a pointer into an array across the call
that reallocated it. RFC 0043 makes ownership part of Safe C: a pointer
declared `WEAVEC_OWNED` is the one responsible for its block, and the
checker follows each owner through the function that holds it, rejecting a
use after a move or a release, a leak on any path, and a borrow used after
what it pointed into was released or replaced (`WEAVEC_DROPS` marks a
destructor of a struct the caller keeps). A pointer field that is not an
owner is a reference, tested when it is loaded. Nothing the runtime does is
removed on the rules' strength. The safe and typed tiers are one
(`-fweavec-safe`), and `weavec-cc --weavec-explain <rule>` prints each rule
with its rewrite. The stages:

- [x] **S0 The rules on paper** against cJSON, sds, zlib and the `lzw`
      port (findings M1–M20; amendments 5, 6, 8 and 9 adopt some).
- [x] **S1 One safe tier**: `-fweavec-typed` removed.
- [x] **S2–S4 Owners, fields and slots, borrows**: the seven rules.
- [x] **S5 References** tested when loaded; the ledger (version 6) and the
      audit (version 2).
- [x] **S6 Explanations and the guide.**
- [x] **S7 Conversions of the eight projects and the port, the C and Rust
      port, seeded bugs**; the held-out projects (G4) not run.
- [ ] **Next**: the S0 findings not built (ownership by outcome and by flag,
      owners inside their block, init parameters), reference tests out of
      hot loops, safe C++ on owners, borrows and references.

## Now: RFC 0042 — Typed memory

Design: [RFC 0042 — Typed memory](rfcs/0042-typed-memory.md) (Accepted,
built). A unit compiled with `-fweavec-typed` relies on typed memory, as
safe Rust relies on its types' invariants: every pointer in memory
satisfies its declared bound, the rules make every store of a typed
function keep that true, and loads are believed. Code that is not typed is
trusted to keep it, listed by `weavec-cc --weavec-audit`, and checked under
`-fweavec-safe-checks=full` and `verify`. Measured: the six conversions
run at 1.45 times the reference build (geometric mean; Safe C 1.82), zlib
and tinyexpr within 1.15; a port of Go's `compress/lzw` runs within 1.18
times its Rust port. The stages:

- [x] **S0**: jsmn's comparison cost explained (the saturating form).
- [x] **S1 The checker**: `-fweavec-typed`, the added rules, field
      annotations.
- [x] **S2 Loads** believed, with `verify`'s monitors and the ledger.
- [x] **S3 Stores and calls** that keep memory typed.
- [x] **S4 Strings and library calls.**
- [x] **S5 The comparison's form**; frames of typed locals untracked.
- [x] **S6 The runtime**: release records out of the block, referenced
      released blocks cleared at sweeps.
- [x] **S7 Conversions, two held-out projects, a port into typed C and
      into Rust**; gates T6 (cost) and T2 (lines) not fully met.
- [ ] **Next**: comparisons as loop ranges, typed callbacks and `void *`,
      the allocator's cost.

## Now: RFC 0041 — Safe C

Design: [RFC 0041 — Safe C](rfcs/0041-safe-c.md) (Accepted, in progress).
WeaveC makes unmodified C safe at run time, but gives nothing to iterate on
at compile time and costs 1.7 to 1.9 times the reference build, so a
project choosing between rewriting in Rust and staying in C has no reason
to rewrite into C. RFC 0041 adds safe functions: C checked against a few
local rules at compile time, with errors an agent can act on, spatially
safe by bounds that its pointers carry (the model of Clang's
`-fbounds-safety`) and temporally safe by the existing sweep, with bounds
checks in place of guards. Guarded code is unchanged; C++ comes in a later
RFC. The stages:

- [x] **S0 The rules on paper**: jsmn and sds converted against the draft,
      Snappy for C++ (G0 passed with amendments).
- [x] **S1 The annotations and the checker**, the rules' tests.
- [x] **S2 Bounds in IR**, guard replacement, the ledger's reason.
- [x] **S3 The boundary**: entry validation, escaping parameters,
      heap-placed locals, `WEAVEC_BORROWED` fields.
- [x] **S4 Library calls** keep their guards (amendment 7).
- [x] **S5 Conversions by agents** of jsmn, sds, cJSON, linenoise, zlib
      (G2 met for three of five).
- [x] **S6 Held-out conversions** (inih, tinyexpr: G3 met) and the gates
      on darwin-arm64.
- [ ] **The cost gate G5** (not met: safe C costs what guarded C does) and
      the gates on linux-aarch64 and linux-x86_64 (CI).

## Now: RFC 0040 — Reports you can act on

Design: [RFC 0040 — Reports you can act on](rfcs/0040-actionable-reports.md)
(Accepted, in progress). WeaveC's reports named the failing access but not
where its memory came from: a use after free named neither the `malloc` nor
the `free`, a double free named no location, a stack or global overflow did
not name the object, and the opt-in backtrace was unsymbolized. RFC 0040
first makes the weekly corpus run green on all three platforms (settling a
`heap-use-after-free` report in CPython's suite on Linux) and finishes RFC
0038 and RFC 0039, then records each heap block's allocation and release
stacks, symbolizes stacks in the process, names locals and globals, gives
bad releases their own kinds and locations, and writes reports as JSON for
tools. The stages:

- [x] **The RFC.**
- [ ] **S0 The gates**: the darwin runners' failures fixed (libevent's
      CMake policy and `pipe2`, libuv's network tests, SQLite's timed lock
      tests); the CPython report reproduced under load and triaged, from
      this RFC's stacks, as a race in CPython's `drop_gil` at shutdown; the
      weekly run green, RFC 0038 and RFC 0039 Implemented.
- [x] **S1 Stacks and the stack store.**
- [x] **S2 Heap stacks.** Their cost is measured in S6.
- [x] **S3 Symbolization**: `weavec-cc --weavec-symbolize`, `atos`, line
      tables.
- [x] **S4 Reports**: heap, frames, globals, releases, faults.
- [x] **S5 JSON and the scripts**, untracked memory, the deletions.
- [ ] **S6 Fresh iterations** on all three platforms, the documentation,
      the cost gates.

## Now: RFC 0039 — C++

Design: [RFC 0039 — C++](rfcs/0039-cxx.md) (Accepted, in progress). WeaveC compiles C++
as Clang does, without guards, so any program with a C++ unit is outside
the guarantee. A prototype that only lifted that exclusion ran the C++
standard library's containers, strings, streams, exceptions and threads
unchanged in 0.7 times ASan's CPU time, but left library calls under RAII
unguarded, cleared the whole stack's shadow at every exception, and could
not tell `new[]` from `new` or `malloc`. RFC 0039 adds `weavec-c++`,
guards calls that can unwind, marks only the frames an exception unwinds,
checks `new` and `delete` forms and sizes, implements ASan's container
annotations, reports the C++ library's hardening checks, and guards the
C++ library's buffer members through the library table. The stages:

- [x] **S0 The RFC**, the C++ corpus, programs and `sealed39` pins, the C++
      detection baseline.
- [x] **S1 The driver**: `weavec-c++`, C++ units guarded, hardening macros.
- [x] **S2 Calls that unwind**: `invoke` in every pass.
- [x] **S3 Exceptions**: the `exits` and `unwinds` clauses, the unwinder's
      interposers, landing pads, per-thread color loss.
- [x] **S4 `new` and `delete`, containers, hardening reports, the C++
      library's rows.**
- [x] **S5 The ledger, reports and unsafe regions.**
- [x] **S6 The gates and toolchains**: eight C++ projects (`fresh39`), the
      STL program, ninja, CMake, Cppcheck and DuckDB in the programs tier,
      `CXX` for every build.
- [ ] **S7 Fresh iterations** on all three platforms, `sealed39`, the
      documentation (darwin-arm64 and linux-aarch64 measured, `sealed39` on
      darwin-arm64; linux-x86_64, the release runners and the weekly corpus
      run need CI).

## Now: RFC 0038 — Drop-in everywhere

Design: [RFC 0038 — Drop-in everywhere](rfcs/0038-drop-in-everywhere.md)
(Accepted; implemented on `rfc0038-drop-in-everywhere`, its _Measured
gates_ recorded for darwin-arm64 and linux-aarch64, and Implemented once
linux-x86_64, the release runners and the weekly corpus run pass). A
measurement of v0.18.0 found the drop-in promise broken on
Linux for any program with a shared library built by `weavec-cc` (each
image carried its own runtime: a version script or `-Bsymbolic` crashed
the program, and zlib, zlib-ng and curl failed their suites), false traps
on untuned code (git's `strcspn` on a buffer it never reads past, a color
root the optimizer formed in zstd), a fault handler that spun once a
program restored it, a weekly corpus run that never measured Linux, and no
way to install WeaveC without building it. RFC 0038 makes the runtime a
shared library every image links, checks a scanning call over the bytes it
read, takes a color check's root from the pointer the program computed
from, measures every platform weekly with a tier of large untuned
programs, and ships self-contained toolchains. The stages:

- [x] **S0 The RFC.**
- [x] **S1 The shared runtime**: `libweavec_rt.so.38`/`.dylib`, the export
      list, the link line and `-fweavec-rpath`, the owner protocol and the
      allocator archive deleted; darwin-arm64 and linux-aarch64.
- [x] **S2 Calls over what they read**: the `scan` clause, the search rows,
      the `strchr`/`memchr` wrappers deleted, the library cases.
- [x] **S3 Roots of color checks**: zstd's root (a select of a hoisted
      `dictBase - delta` and `base`), roots through phis, selects and
      integer arithmetic.
- [x] **S4 The fault handler.**
- [x] **S5 The gates**: platforms, prerequisites, triage verdicts, the
      programs tier, the workflows.
- [x] **S6 Toolchains**: static LLVM on Linux, bundled LLVM libraries on
      Darwin, the relocatable layout, release archives, the image and the
      formula.
- [ ] **S7 Fresh iterations**: the gates on all three platforms (darwin-arm64
      and linux-aarch64 measured; linux-x86_64 and the release runners
      need CI), the documentation.

## Done: RFC 0037 — A complete sweep and object colors

Design: [RFC 0037 — A complete sweep and object colors](rfcs/0037-complete-sweep-and-colors.md)
(Implemented; its _Measured gates_ record the tree it shipped in, and RFC
0038 measured its Linux gates and replaced its per-image dispatch table).
The sweep reads every pointer v0.17.0's missed (vector registers, coroutine
stacks, thread-specific data, read-only memory, file pages written back);
the allocator has thread caches and spare regions and marks in parallel;
the guards' holes are closed and library calls wrapped; faults are
reported; and jumps into a neighbor (object colors in the shadow) and uses
after return (returned frames stay poisoned) trap.

## Done: RFC 0036 — Deterministic heap reuse

Design: [RFC 0036 — Deterministic heap reuse](rfcs/0036-deterministic-heap-reuse.md)
(Implemented; its _Measured gates_ record the tree it shipped in, and RFC
0037 completes the sweep's roots and supersedes its pacing default). A
released heap block is reused only after a sweep finds no pointer to it, so
a use after free or a double free traps however much the program allocated
since; a fortified call's object size is checked; code written for ASan
sees ASan's macros and interface; locals whose lifetimes never overlap
share frame space. Deleted: the advisory analysis, the `weavec` tool, and
every annotation but `WEAVEC_UNSAFE`.

## Done: RFC 0035 — Guard by default

Design: [RFC 0035 — Guard by default](rfcs/0035-guard-by-default.md)
(Implemented; its _Measured gates_ record the final tree, with the run
time of interpreters (Lua, cJSON) and the build time of some units carried
forward; RFC 0036 supersedes its quarantine's reuse after a byte budget
and its advisory analysis, and amends its frame layout).
`weavec-cc` guards every memory access in LLVM IR against the runtime's
shadow memory, and removes a guard only where a local rule proves it
redundant (in bounds of a local or a global, covered by a dominating guard,
merged in a block, a loop whose whole range is checked once); the passes
lay out stack frames and globals with redzones, so use after scope traps;
the shadow says what is addressable, so untracked memory is fast; Clang's
`array-bounds` checks keep typed indexes in bounds; library calls are
guarded from the library table or redirected to checked versions. Deleted:
the checks inserted through Sema, deferred CodeGen, the check planner and
the prelude, unit records and the link step, the require levels, and the
analysis ledger.

## Later

- Objective-C and Objective-C++, which pass through to Clang unchanged.
- Programs' own allocators (pymalloc, pools): their objects share the
  shadow of the blocks they are carved from.
- Windows; signed and notarized macOS toolchains; Debian and RPM packages.
- From RFC 0037's _Future work_: concurrent marking, if parallel marking
  leaves pauses too long; pointers checked where they leave a function,
  and colors carried by a moved pointer; hardware memory tagging (Arm MTE,
  Apple MIE) where the processor has it.
- Concurrency safety beyond assumption A3 (data races between a guard and its access).

## History

- **RFC 0045 — Type-stable memory** (2026-11-14): a safe unit's blocks are
  reused at once by their own type; believed references and trusted
  standard-library clones; the checked mode keeps every guard.
- **RFC 0044 — Safe C++** (2026-11-13): owners, borrows, views and
  references; `owner-moved`, `stale-borrow`, `call-alias`, `owner-path`,
  `escape`; 82 of 110 seeded temporal bugs stop the compile.

The milestones that built the model, the analysis and the guards. RFC 0036
deletes the analysis and marks the RFCs that specified it (0001–0017, 0031) Superseded; they stay as the record of what was built and measured.

| Milestone                             | Design                                                                                                                | Scope                                                                                  | RFC status                                                                                                   |
| ------------------------------------- | --------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| 0 Scaffolding                         | [RFC 0001](rfcs/0001-ownership-model.md)                                                                              | Layered libraries, pipeline, annotation header, the ownership model                    | Superseded by RFC 0036; guarantee replaced by RFC 0030, then RFC 0035                                        |
| 1 Intra-procedural checking           | [RFC 0002](rfcs/0002-intraprocedural-checking.md)                                                                     | CFG dataflow, alias relation, moves, loans, lifetimes                                  | Superseded by RFC 0036                                                                                       |
| 2 Signature inference and annotations | [RFC 0003](rfcs/0003-signature-inference.md), [RFC 0004](rfcs/0004-unsafe-boundaries.md)                              | Summaries, reconciliation, raw pointers, unsafe regions, indirect calls                | Superseded by RFC 0036; unsafe regions live on in RFC 0035                                                   |
| 3–4 Compiler driver and whole program | [RFC 0005](rfcs/0005-whole-program-analysis.md)                                                                       | `weavec-cc`, `--whole-program`                                                         | Superseded by RFC 0036; `weavec-cc` lives on, records and the link step deleted by RFC 0035                  |
| 5 Precision                           | [RFC 0006](rfcs/0006-precision.md)                                                                                    | Non-lexical loans, condition facts, element places, outcome-conditional summaries      | Superseded by RFC 0036                                                                                       |
| 6 Resource lifecycle                  | [RFC 0007](rfcs/0007-resource-lifecycle.md)                                                                           | Leaks, release families, owned fields                                                  | Superseded by RFC 0036                                                                                       |
| 7 Pointer validity                    | [RFC 0008](rfcs/0008-pointer-validity.md)                                                                             | Null dereferences, uninitialized pointers, invalid releases, replaced values           | Superseded by RFC 0036                                                                                       |
| 8 Value-conditional behavior          | [RFC 0009](rfcs/0009-value-conditional-behaviour.md)                                                                  | Scalar facts, guarded effects, inferred `noreturn`                                     | Superseded by RFC 0036                                                                                       |
| 9 Shared ownership                    | [RFC 0010](rfcs/0010-shared-ownership.md)                                                                             | Reference counts, ownership by outcome                                                 | Superseded by RFC 0036                                                                                       |
| 10–11 Spatial safety                  | [RFC 0011](rfcs/0011-spatial-safety.md), [RFC 0012](rfcs/0012-spatial-safety-strings-and-fields.md)                   | Derived pointers, extents, strings, sized fields, assumptions                          | Superseded by RFC 0036                                                                                       |
| 12 Interprocedural heap state         | [RFC 0013](rfcs/0013-interprocedural-heap-state.md)                                                                   | Heap postconditions, value snapshots                                                   | Superseded by RFC 0036                                                                                       |
| 13 Pointer identity and call effects  | [RFC 0014](rfcs/0014-pointer-identity-and-call-effects.md)                                                            | Function values, callback specialization, pointer-copy identity                        | Superseded by RFC 0036                                                                                       |
| 14 Arrays and containers              | [RFC 0015](rfcs/0015-array-and-container-ownership.md)                                                                | Element selectors, ranges, container ownership                                         | Superseded by RFC 0036                                                                                       |
| 15 Compositional call checking        | [RFC 0016](rfcs/0016-compositional-call-checking.md)                                                                  | Call contexts under caller alias relationships                                         | Superseded by RFC 0036                                                                                       |
| 16 C integers and spatial checking    | [RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md)                                                       | Target integer semantics, typed expressions, VLAs, flexible arrays                     | Superseded by RFC 0036                                                                                       |
| 17–27 Checked mode                    | [RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md)–[RFC 0029](rfcs/0029-compositional-recursive-workflows.md) | Opt-in checked functions with safety contracts                                         | Superseded by RFC 0030; the mode is deleted                                                                  |
| 28 Prove or trap                      | [RFC 0030](rfcs/0030-prove-or-trap.md)                                                                                | One outcome per safety facet, the library table, pointer kinds, function-pointer slots | Implemented; its enforcement superseded by RFC 0035 and its analysis by RFC 0036; the library table lives on |
| 29 The object engine                  | [RFC 0031](rfcs/0031-object-engine.md)                                                                                | Facts on abstract objects and symbolic values, so every alias sees every fact          | Superseded by RFC 0036, which deletes the analysis and `weavec`                                              |
| 30 Runtime enforcement                | [RFC 0032](rfcs/0032-runtime-enforcement.md)                                                                          | The runtime's allocator and quarantine, guards                                         | Implemented; guards and registration superseded by RFC 0035, the quarantine's reuse by RFC 0036              |
| 31 Drop-in by default                 | [RFC 0033](rfcs/0033-drop-in-by-default.md)                                                                           | Correct untuned projects build and run; one runtime per process                        | Implemented; link modes and records superseded by RFC 0035                                                   |
| 32 Fast enforcement                   | [RFC 0034](rfcs/0034-fast-enforcement.md)                                                                             | Shadow memory, confirmed errors, the work budget                                       | Implemented; guard lowering superseded by RFC 0035                                                           |
| 33 Guard by default                   | [RFC 0035](rfcs/0035-guard-by-default.md)                                                                             | Every access guarded in LLVM IR, local removal rules, redzones, the advisory analysis  | Implemented; the quarantine's budget and the analysis superseded by RFC 0036, the frame layout amended       |

RFCs 0030–0034 made the analysis decide what the build enforced. Measured
after RFC 0034, the runtime did the catching and the analysis caused the
false stops, the missed bugs (wrong proofs removed needed guards) and most
of the build cost, which is why RFC 0035 guards every access and makes the
analysis advisory. Measured after RFC 0035, the advisory analysis reported
no bug worth fixing on untuned code and found none of the bugs the guards
miss, while a use after free passed once the quarantine had recycled its
block, which is why RFC 0036 deletes the analysis and reuses released
memory only after a sweep finds no pointer to it.

## Ongoing

- Corpus testing against real C projects pinned by SHA
  (`scripts/corpus-gate.py` over `test/corpus/`: a ratchet in
  `test/corpus/expected.json` and a verdict for every guard failure in
  `test/corpus/triage.json`); `--quick` on every pull request, `--full`
  weekly.
- Windows support.
