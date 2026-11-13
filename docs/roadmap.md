# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

The design today is [RFC 0043](rfcs/0043-ownership.md), [RFC 0042](rfcs/0042-typed-memory.md) and [RFC 0041](rfcs/0041-safe-c.md) for safe C, and [RFC 0038](rfcs/0038-drop-in-everywhere.md), over the
sweep and colors of [RFC 0037](rfcs/0037-complete-sweep-and-colors.md), the
reuse after a sweep of [RFC 0036](rfcs/0036-deterministic-heap-reuse.md)
and the guards of [RFC 0035](rfcs/0035-guard-by-default.md). The
milestones before it are summarized under _History_; their RFCs keep the
detail, and the per-RFC validation records of the milestones before RFC
0030 remain in the repository history at tag `v0.10.0`.

## Next: RFC 0044 — Safe C++

Design: [RFC 0044 — Safe C++](rfcs/0044-safe-cxx.md) (Accepted, not built). Safe C gives
an agent compile errors for bounds, conversions and ownership; C++, the
language a rewrite would more likely target, has only the run-time half:
`weavec-c++` stops each of twenty probe bugs when a test reaches it and
rejects none when it compiles, and `-fweavec-safe` is an error in C++. RFC
0044 makes the flag work: safe C++ functions are checked against local
rules over owners (objects), borrows and views (references, iterators,
`std::string_view`, types that hold them) and references (pointer members,
left to the guards), rejecting a use after `std::move`, a borrow used after
its container changed or died, a view returned or stored past what it
points into, and raw `new`, `delete` and pointer arithmetic. Nothing at run
time changes: safe C++ is a subset, costs what guarded C++ costs until a
later RFC spends the rules, does not change what an object owns through a
raw pointer member, and trusts the standard library. The stages:

- [x] **The RFC**, accepted as drafted on 2026-11-12.
- [ ] **S0 On paper, and probes**: the rules applied by hand to three
      files, a prototype dataflow over the probe, the frozen sets of bugs
      and of correct functions, the ports and conversions chosen.
- [ ] **S1 Cleanups**: the rule `cxx` deleted, the ownership checker split
      into a shared engine and C's front, the flag in C++.
- [ ] **S2 Expression rules**; **S3 Kinds of types and moves**;
      **S4 Borrows**; **S5 Views**.
- [ ] **S6 Explanations and the guide.**
- [ ] **S7 Ports against Rust, conversions and seeded bugs**; the gates.

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
