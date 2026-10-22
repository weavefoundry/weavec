# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

The design today is [RFC 0036](rfcs/0036-deterministic-heap-reuse.md)
over the guards of [RFC 0035](rfcs/0035-guard-by-default.md). The
milestones before it are summarised under _History_; their RFCs keep the
detail, and the per-RFC validation records of the milestones before RFC
0030 remain in the repository history at tag `v0.10.0`.

## Now: RFC 0036 — Deterministic heap reuse

Design: [RFC 0036 — Deterministic heap reuse](rfcs/0036-deterministic-heap-reuse.md)
(Accepted; being implemented on `rfc0036-deterministic-safety`, and
Implemented once its _Acceptance gates_ pass on the final tree). A released
heap block is reused only after a _sweep_ (the program's threads stopped,
its stacks, registers, writable data and live heap scanned conservatively)
finds no pointer to it, so a use after free or a double free traps however
much memory the program allocated since; sweeps are paced to a small share
of the run time. A fortified call's object size is checked; code written
for ASan sees ASan's macros and interface; locals whose lifetimes never
overlap share frame space; the globals of shared libraries and bundles are
registered with the process's runtime; the number parsers no longer need a
terminator they never read. Deleted: the advisory analysis (the `weavec`
tool, `-fweavec-diagnose`, `weavec --whole-program`, `lib/Analysis`, all of
`lib/Core` but the library table), every annotation but `WEAVEC_UNSAFE`,
and their diagnostics.

The RFC's first draft also made spatial safety and use after return
deterministic (bounds from each pointer's provenance, locals in frame
blocks from the arena). It was implemented on the same branch and measured
at twice v0.16.0's run time (geometric mean 3.1× the reference compiler,
QuickJS 10.8×); the revision keeps the cheap part and records the
measurement of each mechanism in its _Alternatives_. The stages:

- [x] **S0 The RFC**: the revision.
- [x] **S1 Remove the first draft's spatial and frame mechanisms**:
      provenance bounds, escapes and the escape registry, pointer slots,
      frame blocks, the `setjmp` hook, the globals table; RFC 0035's native
      frames back.
- [x] **S2 Shared frame space** (`StackLifetime`).
- [x] **S3 The sweep's cost**: pacing, the image table, address-order
      reuse.
- [x] **S4 Cases, lit and runtime tests** for the revised scope.
- [x] **S5 Fresh iterations** on darwin-arm64: the motivation's projects,
      the corpus, the gates (recorded in the RFC's _Measured gates_), the
      documentation.
- [ ] **Linux** through CI, then the RFC is Implemented (QuickJS's run
      time, the cost of the ASan contract, is carried forward).

RFC 0035's plan for RFC 0036, ownership as contracts checked at function
entry, is withdrawn: it would remove guards on the strength of inferred
facts, which would have to be right where the analysis measured 62 false
findings in 69 (RFC 0036 _Alternatives_).

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

## Next

RFC 0036's _Future work_:

- Spatial determinism (an access checked against its pointer's object) and
  use after return, under a cost budget measured before the design.
- Concurrent marking, so that a sweep does not stop the program for its
  whole duration.
- Hardware memory tagging (Arm MTE, Apple MIE) where the processor has it.
- Adoption: relocatable installs, packages, a vendored `weavec.h`.

## Later

- Concurrency safety beyond assumption A3 (data races between a guard and its access).
- C++ and Objective-C, which pass through to Clang unchanged today.

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
| 7 Pointer validity                    | [RFC 0008](rfcs/0008-pointer-validity.md)                                                                             | Null dereferences, uninitialised pointers, invalid releases, replaced values           | Superseded by RFC 0036                                                                                       |
| 8 Value-conditional behaviour         | [RFC 0009](rfcs/0009-value-conditional-behaviour.md)                                                                  | Scalar facts, guarded effects, inferred `noreturn`                                     | Superseded by RFC 0036                                                                                       |
| 9 Shared ownership                    | [RFC 0010](rfcs/0010-shared-ownership.md)                                                                             | Reference counts, ownership by outcome                                                 | Superseded by RFC 0036                                                                                       |
| 10–11 Spatial safety                  | [RFC 0011](rfcs/0011-spatial-safety.md), [RFC 0012](rfcs/0012-spatial-safety-strings-and-fields.md)                   | Derived pointers, extents, strings, sized fields, assumptions                          | Superseded by RFC 0036                                                                                       |
| 12 Interprocedural heap state         | [RFC 0013](rfcs/0013-interprocedural-heap-state.md)                                                                   | Heap postconditions, value snapshots                                                   | Superseded by RFC 0036                                                                                       |
| 13 Pointer identity and call effects  | [RFC 0014](rfcs/0014-pointer-identity-and-call-effects.md)                                                            | Function values, callback specialisation, pointer-copy identity                        | Superseded by RFC 0036                                                                                       |
| 14 Arrays and containers              | [RFC 0015](rfcs/0015-array-and-container-ownership.md)                                                                | Element selectors, ranges, container ownership                                         | Superseded by RFC 0036                                                                                       |
| 15 Compositional call checking        | [RFC 0016](rfcs/0016-compositional-call-checking.md)                                                                  | Call contexts under caller alias relationships                                         | Superseded by RFC 0036                                                                                       |
| 16 C integers and spatial checking    | [RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md)                                                       | Target integer semantics, typed expressions, VLAs, flexible arrays                     | Superseded by RFC 0036                                                                                       |
| 17–27 Checked mode                    | [RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md)–[RFC 0029](rfcs/0029-compositional-recursive-workflows.md) | Opt-in checked functions with safety contracts                                         | Superseded by RFC 0030; the mode is deleted                                                                  |
| 28 Prove or trap                      | [RFC 0030](rfcs/0030-prove-or-trap.md)                                                                                | One outcome per safety facet, the library table, pointer kinds, function-pointer slots | Implemented; its enforcement superseded by RFC 0035 and its analysis by RFC 0036; the library table lives on |
| 29 The object engine                  | [RFC 0031](rfcs/0031-object-engine.md)                                                                                | Facts on abstract objects and symbolic values, so every alias sees every fact          | Superseded by RFC 0036, which deletes the analysis and `weavec`                                              |
| 30 Runtime enforcement                | [RFC 0032](rfcs/0032-runtime-enforcement.md)                                                                          | The runtime's allocator and quarantine, guards                                         | Implemented; guards and registration superseded by RFC 0035, the quarantine's reuse by RFC 0036              |
| 31 Drop-in by default                 | [RFC 0033](rfcs/0033-drop-in-by-default.md)                                                                           | Correct untuned projects build and run; one runtime per process                        | Implemented; link modes and records superseded by RFC 0035                                                   |
| 32 Fast enforcement                   | [RFC 0034](rfcs/0034-fast-enforcement.md)                                                                             | Shadow memory, confirmed errors, the work budget                                       | Implemented; guard lowering superseded by RFC 0035                                                           |
| 33 Guard by default                   | [RFC 0035](rfcs/0035-guard-by-default.md)                                                                             | Every access guarded in LLVM IR, local removal rules, redzones, the advisory analysis  | Implemented; the quarantine's budget and the analysis superseded by RFC 0036, the frame layout amended         |

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
