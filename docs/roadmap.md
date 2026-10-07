# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

The design today is [RFC 0035](rfcs/0035-guard-by-default.md). The
milestones before it are summarised under *History*; their RFCs keep the
detail, and the per-RFC validation records of the milestones before RFC
0030 remain in the repository history at tag `v0.10.0`.

## Done: RFC 0035 — Guard by default

Design: [RFC 0035 — Guard by default](rfcs/0035-guard-by-default.md)
(Implemented; its *Measured gates* record the final tree, with the run
time of interpreters (Lua, cJSON) and the build time of some units carried
forward).
`weavec-cc` guards every memory access in LLVM IR against the runtime's
shadow memory, and removes a guard only where a local rule proves it
redundant (in bounds of a local or a global, covered by a dominating guard,
merged in a block, a loop whose whole range is checked once); the passes
lay out stack frames and globals with redzones, so use after scope traps;
the shadow says what is addressable, so untracked memory is fast; Clang's
`array-bounds` checks keep typed indexes in bounds; library calls are
guarded from the library table or redirected to checked versions. The
analysis is advisory (`weavec`, `weavec --whole-program`,
`weavec-cc -fweavec-diagnose`) and changes no generated code. Deleted: the
checks inserted through Sema, deferred CodeGen, the check planner and the
prelude, unit records and the link step, the require levels, and the
analysis ledger. The stages landed on `rfc0035-guard-by-default` and ship
as one change; the RFC's *Implementation amendments* record the decisions:

- [x] **S0 Tests first**: `fresh35` and `sealed35`, the wrong proofs and
      false stops as cases, the CVE and blind sets.
- [x] **S1 The runtime**: the shadow encoding and its window, the
      allocator's shadow, the entry points.
- [x] **S2 The passes**: accesses, guards, library calls, unsafe regions,
      array bounds, the enforcement ledger.
- [x] **S3 Frames and globals.**
- [x] **S4 Removing guards**, loop ranges included.
- [x] **S5 The driver** and the deletion of the emission path and records.
- [x] **S6 The advisory analysis.**
- [x] **S7 Tests, the runner, the corpus gate, CI.**
- [x] **S8 Fresh iterations, gates, the sealed run, documentation.**

## Next: RFC 0036 — Ownership as checked contracts

- Inferred function contracts (a borrowed parameter is live and has `n`
  bytes, a callee does not release it) checked once at entry, so the
  body's guards go; contracts proven at every caller drop the entry check.
- Ownership and extent annotations as contracts.

## Planned: RFC 0037 — Adoption

- Fingerprinted baselines and reasoned suppressions.
- `weavec.toml`, with path scoping and API overlays.
- `weavec suggest --apply` for the analysis's fix-its.
- A vendored `weavec.h`.
- Relocatable installs.

## Later

- Deterministic heap temporal safety: freed memory reused only after a
  sweep finds no pointer to it.
- Use after return, with a fake stack for frames whose locals escape.
- Hardware memory tagging (Arm MTE, Apple MIE) where the processor has it.
- Concurrency safety beyond assumption A3 (data races between a guard and its access).
- C++ and Objective-C, which pass through to Clang unchanged today.
- Reading `lifetimebound`, `noescape` and `lifetime_capture_by`.

## History

The milestones that built the model and the analysis:

| Milestone | Design | Scope | RFC status |
| --- | --- | --- | --- |
| 0 Scaffolding | [RFC 0001](rfcs/0001-ownership-model.md) | Layered libraries, pipeline, annotation header, the ownership model | Accepted; guarantee replaced by RFC 0030, then RFC 0035 |
| 1 Intra-procedural checking | [RFC 0002](rfcs/0002-intraprocedural-checking.md) | CFG dataflow, alias relation, moves, loans, lifetimes | Implemented; severities amended by RFC 0030 |
| 2 Signature inference and annotations | [RFC 0003](rfcs/0003-signature-inference.md), [RFC 0004](rfcs/0004-unsafe-boundaries.md) | Summaries, reconciliation, raw pointers, unsafe regions, indirect calls | Implemented; unsafe regions amended by RFCs 0030 and 0035 |
| 3–4 Compiler driver and whole program | [RFC 0005](rfcs/0005-whole-program-analysis.md) | `weavec-cc`, `--whole-program` | Implemented; records and the link step deleted by RFC 0035 |
| 5 Precision | [RFC 0006](rfcs/0006-precision.md) | Non-lexical loans, condition facts, element places, outcome-conditional summaries | Accepted |
| 6 Resource lifecycle | [RFC 0007](rfcs/0007-resource-lifecycle.md) | Leaks, release families, owned fields | Accepted; leak rules amended by RFC 0030 |
| 7 Pointer validity | [RFC 0008](rfcs/0008-pointer-validity.md) | Null dereferences, uninitialised pointers, invalid releases, replaced values | Accepted; severities amended by RFC 0030 |
| 8 Value-conditional behaviour | [RFC 0009](rfcs/0009-value-conditional-behaviour.md) | Scalar facts, guarded effects, inferred `noreturn` | Accepted |
| 9 Shared ownership | [RFC 0010](rfcs/0010-shared-ownership.md) | Reference counts, ownership by outcome | Accepted |
| 10–11 Spatial safety | [RFC 0011](rfcs/0011-spatial-safety.md), [RFC 0012](rfcs/0012-spatial-safety-strings-and-fields.md) | Derived pointers, extents, strings, sized fields, assumptions | Accepted |
| 12 Interprocedural heap state | [RFC 0013](rfcs/0013-interprocedural-heap-state.md) | Heap postconditions, value snapshots | Accepted |
| 13 Pointer identity and call effects | [RFC 0014](rfcs/0014-pointer-identity-and-call-effects.md) | Function values, callback specialisation, pointer-copy identity | Implemented; callback globals replaced by RFC 0030 slots |
| 14 Arrays and containers | [RFC 0015](rfcs/0015-array-and-container-ownership.md) | Element selectors, ranges, container ownership | Implemented |
| 15 Compositional call checking | [RFC 0016](rfcs/0016-compositional-call-checking.md) | Call contexts under caller alias relationships | Implemented |
| 16 C integers and spatial checking | [RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md) | Target integer semantics, typed expressions, VLAs, flexible arrays | Implemented |
| 17–27 Checked mode | [RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md)–[RFC 0029](rfcs/0029-compositional-recursive-workflows.md) | Opt-in checked functions with safety contracts | Superseded by RFC 0030; the mode is deleted |
| 28 Prove or trap | [RFC 0030](rfcs/0030-prove-or-trap.md) | One outcome per safety facet, the library table, pointer kinds, function-pointer slots | Implemented; its enforcement (inserted checks, require levels, records) superseded by RFC 0035 |
| 29 The object engine | [RFC 0031](rfcs/0031-object-engine.md) | Facts on abstract objects and symbolic values, so every alias sees every fact | Implemented; the analysis behind `weavec` |
| 30 Runtime enforcement | [RFC 0032](rfcs/0032-runtime-enforcement.md) | The runtime's allocator and quarantine, guards | Implemented; guards and registration superseded by RFC 0035 |
| 31 Drop-in by default | [RFC 0033](rfcs/0033-drop-in-by-default.md) | Correct untuned projects build and run; one runtime per process | Implemented; link modes and records superseded by RFC 0035 |
| 32 Fast enforcement | [RFC 0034](rfcs/0034-fast-enforcement.md) | Shadow memory, confirmed errors, the work budget | Implemented; guard lowering superseded by RFC 0035 |

RFCs 0030–0034 made the analysis decide what the build enforced. Measured
after RFC 0034, the runtime did the catching and the analysis caused the
false stops, the missed bugs (wrong proofs removed needed guards) and most
of the build cost, which is why RFC 0035 guards every access and makes the
analysis advisory.

## Ongoing

- Corpus testing against real C projects pinned by SHA
  (`scripts/corpus-gate.py` over `test/corpus/`: a ratchet in
  `test/corpus/expected.json` and a verdict for every finding in
  `test/corpus/triage.json`); `--quick` on every pull request, `--full`
  weekly.
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
