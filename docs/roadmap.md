# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

The per-RFC validation records and generated results of earlier milestones
were removed by [RFC 0030](rfcs/0030-prove-or-trap.md); they remain in the
repository history at tag `v0.10.0`.

## Now: RFC 0030 — Prove or trap (in progress)

Design: [RFC 0030 — Prove or trap](rfcs/0030-prove-or-trap.md) (Accepted).
One safety semantics: every spatial, null and temporal facet of every
operation is proven, checked by a runtime check `weavec-cc` inserts, a
definite violation, or unresolved or trusted with a reason, all recorded in
a ledger, under the guarantee and assumptions A1–A5 of its *Soundness*
section. Checked mode (RFCs 0018–0029) is deleted. The stages land on the
`rfc0030-prove-or-trap` branch and ship as one change:

- [x] **S0 Oracles and reset.** The golden v0.10.0 oracle, `test/cases`
      with `scripts/run-cases.py`, `test/corpus` with
      `scripts/corpus-gate.py`, and the deletion of generated evidence.
- [x] **S1 Retire checked mode** by reachability, with diagnostics
      byte-identical to the golden run.
- [x] **S2 Deferred CodeGen**, with objects byte-identical to Clang's when
      no check is inserted (`scripts/codegen-identity.py`).
- [x] **S3 Ledger and semantics.** Sites, facets and outcomes, certainty,
      sound defaults for unknown code, `WEAVEC_UNSAFE`/`WEAVEC_ASSUME`/require
      levels, budgets, the JSON and SARIF ledger, the check planner.
- [ ] **S4 Library table.** `LibrarySpec` replaces the old library models,
      with callbacks, threads and signals, `allocation-failure` and the leak
      rule.
- [x] **S5 Emission.** Checks inserted through Sema in the four modes, the
      report-mode runtime, lowered-violation traps, zero-initialisation and
      the precompiled-header fallback.
- [ ] **S6 Kinds.** Declared and inferred pointer kinds consumed by the
      engine, slot kinds, must-access requirements, field invariants.
- [ ] **S7 Temporal precision.** Outcome-keyed effects with `lossy` bits,
      function-pointer slots per unit and program-wide, boundary invariants
      and their propagation.
- [ ] **S8 Link and wrap-up.** Format-28 unit records, the link step
      (declaration verification, reliance checks, `unanalyzed-input`), CLI
      cleanup, documentation, RFC statuses and CI wiring.

The RFC becomes Implemented when every acceptance gate (G1–G15, H1–H3)
passes on the final tree.

## Next: RFC 0031 — Residual enforcement and precision (planned)

Sized by the ledger's unresolved-reason histogram:

- a replacement engine behind the `SafetyEngine` seam (semantic IR,
  symbolic heap, relational domain);
- a temporal runtime backstop: a quarantine allocator, and Arm MTE or Apple
  MIE where the hardware allows;
- an ABI-compatible heap-bounds runtime for the residual `unknown-extent`
  sites, such as header-before-pointer strings and `container_of`;
- precision where the histogram shows it pays: state machines, guard
  functions, array cells;
- proof-dependency tracking, for a sharper blame property.

## Next: RFC 0032 — Adoption (planned)

- Format-28 records embedded in object sections, so archives, shared
  libraries, ccache and LTO carry them.
- Fingerprinted baselines and reasoned suppressions.
- `weavec.toml`, with path scoping and API overlays.
- `weavec suggest --apply` for the ledger's fix-its.
- A vendored `weavec.h`.
- Relocatable installs.

## Later

- Concurrency safety beyond assumption A4.
- C++ and Objective-C, which pass through to Clang unchanged today.
- Reading `lifetimebound`, `noescape` and `lifetime_capture_by`.

## History

The model and the engine that RFC 0030 builds on:

| Milestone | Design | Scope | RFC status |
| --- | --- | --- | --- |
| 0 Scaffolding | [RFC 0001](rfcs/0001-ownership-model.md) | Layered libraries, pipeline, annotation header, the ownership model | Accepted; guarantee replaced by RFC 0030 |
| 1 Intra-procedural checking | [RFC 0002](rfcs/0002-intraprocedural-checking.md) | CFG dataflow, alias relation, moves, loans, lifetimes | Implemented; severities amended by RFC 0030 |
| 2 Signature inference and annotations | [RFC 0003](rfcs/0003-signature-inference.md), [RFC 0004](rfcs/0004-unsafe-boundaries.md) | Summaries, reconciliation, raw pointers, unsafe regions, indirect calls | Implemented; unknown-callee default and unsafe regions amended by RFC 0030 |
| 3–4 Compiler driver and whole program | [RFC 0005](rfcs/0005-whole-program-analysis.md) | `weavec-cc`, per-object records, the link step, `--whole-program` | Implemented; record format and link step amended by RFC 0030 |
| 5 Precision | [RFC 0006](rfcs/0006-precision.md) | Non-lexical loans, condition facts, element places, outcome-conditional summaries | Accepted |
| 6 Resource lifecycle | [RFC 0007](rfcs/0007-resource-lifecycle.md) | Leaks, release families, owned fields | Accepted; leak rules amended by RFC 0030 |
| 7 Pointer validity | [RFC 0008](rfcs/0008-pointer-validity.md) | Null dereferences, uninitialised pointers, invalid releases, replaced values | Accepted; severities amended by RFC 0030 |
| 8 Value-conditional behaviour | [RFC 0009](rfcs/0009-value-conditional-behaviour.md) | Scalar facts, guarded effects, inferred `noreturn` | Accepted |
| 9 Shared ownership | [RFC 0010](rfcs/0010-shared-ownership.md) | Reference counts, ownership by outcome | Accepted |
| 10–11 Spatial safety | [RFC 0011](rfcs/0011-spatial-safety.md), [RFC 0012](rfcs/0012-spatial-safety-strings-and-fields.md) | Derived pointers, extents, strings, sized fields, assumptions | Accepted; possible overruns and assumptions are runtime checks under RFC 0030 |
| 12 Interprocedural heap state | [RFC 0013](rfcs/0013-interprocedural-heap-state.md) | Heap postconditions, value snapshots | Accepted |
| 13 Pointer identity and call effects | [RFC 0014](rfcs/0014-pointer-identity-and-call-effects.md) | Function values, callback specialisation, pointer-copy identity | Implemented; callback globals replaced by RFC 0030 slots |
| 14 Arrays and containers | [RFC 0015](rfcs/0015-array-and-container-ownership.md) | Element selectors, ranges, container ownership | Implemented |
| 15 Compositional call checking | [RFC 0016](rfcs/0016-compositional-call-checking.md) | Call contexts under caller alias relationships | Implemented |
| 16 C integers and spatial checking | [RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md) | Target integer semantics, typed expressions, VLAs, flexible arrays | Implemented |

Milestones 17–27 built checked mode: opt-in checked functions with safety
contracts, from [RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md)
through [RFC 0029](rfcs/0029-compositional-recursive-workflows.md). Its
complete contracts plateaued at 150 of 1,619 corpus definitions and it could
not analyse Lua within its cost limits, so RFC 0030 superseded those RFCs
and deleted the mode. `WEAVEC_REQUIRE_SAFE` replaces its `WEAVEC_CHECKED`,
and `-fweavec-require=proven` is the fail-closed tier without runtime checks.

## Ongoing

- Corpus testing against real C projects pinned by SHA
  (`scripts/corpus-gate.py` over `test/corpus/`: a ratchet in
  `test/corpus/expected.json` and a verdict for every finding in
  `test/corpus/triage.json`); `--quick` on every pull request, `--full`
  weekly.
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
