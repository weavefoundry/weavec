# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

The per-RFC validation records and generated results of earlier milestones
were removed by [RFC 0030](rfcs/0030-prove-or-trap.md); they remain in the
repository history at tag `v0.10.0`.

## Done: RFC 0030 — Prove or trap

Design: [RFC 0030 — Prove or trap](rfcs/0030-prove-or-trap.md)
(Implemented, as amended by RFCs 0031 and 0032).
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
- [x] **S4 Library table.** `LibrarySpec` replaces the old library models,
      with callbacks, threads and signals, `allocation-failure` and the leak
      rule.
- [x] **S5 Emission.** Checks inserted through Sema in the four modes, the
      report-mode runtime, lowered-violation traps, zero-initialisation and
      the precompiled-header fallback.
- [x] **S6 Kinds.** Declared and inferred pointer kinds consumed by the
      engine, slot kinds, must-access requirements and store groups. The
      counted-field Houdini rounds (§7.6) were the designated first cut and
      did not land: the candidates are inferred and printed by
      `weavec --dump-kinds`, but no field extent becomes exact by an
      invariant.
- [x] **S7 Temporal precision.** Outcome-keyed effects with `lossy` bits,
      function-pointer slots per unit and program-wide, boundary invariants
      and their propagation. The unknown-callee default of §5.1 became
      lazy in the same stage: one record per object, inherited by places
      named after the call, which closed a case where a place first named
      after an unknown call could be proven.
- [x] **S8 Link and wrap-up.** Format-28 unit records, the link step
      (declaration verification, reliance checks, `unanalyzed-input`), CLI
      cleanup, documentation, RFC statuses and CI wiring.

Its open gates G10, G14 and G15 were taken over, and closed, by RFC 0031.

## Done: RFC 0031 — The object engine

Design: [RFC 0031 — The object engine](rfcs/0031-object-engine.md)
(Implemented). Replaced the path-based engine behind the RFC 0030 seam with an
engine whose facts live on abstract objects and symbolic values, so every
alias sees every fact by construction, and replaced its summaries (format
30) and unit records (format 29). It is aimed at the false proofs of
v0.11.0, the false definite errors measured on eleven held-out projects and
the analysis-time blow-ups on large files, and it takes over RFC 0030's
open gates G10, G14 and G15. The stages landed on the
`rfc0031-object-engine` branch and ship as one change; the decisions made
while implementing them are recorded in the RFC's *Implementation
amendments*:

- [x] **S0 Tests first.** Alias probes, held-out repros, object-domain
      cases and the held-out corpus configs.
- [x] **S1 Domain.** Symbols, objects, cells, the zone, distinctness,
      joins, widening, garbage collection and materialisation, in Core.
- [x] **S2 Intraprocedural engine** with site decisions and witnesses.
- [x] **S3 Calls** and format-30 summaries.
- [x] **S4 Temporal completeness.**
- [x] **S5 Records and link** (unit record format 29).
- [x] **S6 Delete** the old engine and its trackers.
- [x] **S7 Fixes and cost.**

Three numeric targets were not met and are carried forward as future work,
with what was measured kept as a ratchet (the RFC's *Gates carried forward*
amendment): the unresolved shares of G6 (to RFC 0032), the build-cost bound
of G12 (to RFC 0033) and G4's count of possible temporal warnings.

## Done: RFC 0032 — Runtime enforcement

Design: [RFC 0032 — Runtime enforcement](rfcs/0032-runtime-enforcement.md)
(Implemented, with its cost gate amended). Every enforcing build links a
runtime whose allocator keeps each heap object's extent and liveness
findable from a pointer, and which tracks escaping stack objects and
globals. Unresolved spatial and temporal facets with a pointer operand
become *guarded*: checked against that object table, with a quarantine as
the temporal backstop. `verify` mode monitors temporal proofs too. The
stages landed on the `rfc0032-runtime-enforcement` branch and ship as one
change; the decisions made while implementing them are recorded in the
RFC's *Implementation amendments*:

- [x] **S0 Tests first.**
- [x] **S1 The runtime**: allocator, quarantine, stack list, global table.
- [x] **S2 Core**: the `guarded` outcome, ledger version 2, record format 30.
- [x] **S3 Access guards**: planner, prelude, emitter, driver.
- [x] **S4 Stack and global objects.**
- [x] **S5 Library calls and releases.**
- [x] **S6 Verify mode and the false proof through array elements** (a
      boundary rule, not a summary: amendment 1).
- [x] **S7 Possible findings, the runner, the corpus gate, documentation.**
- [x] **S8 Gates and cost.**

One target was not met and is carried forward (amendment 3): the default
mode's cost. cJSON runs at 1.66× the reference compiler's build, zlib at
1.85× and the Lua benchmark at 5.94× (the RFC set 2.0×, 1.5× and 2.0×);
without the runtime the three are at 1.14×, 1.00× and 1.11×. The cost is
the number of guards a program executes, so the next step is removing
guards before they run.

Left for later RFCs: static guard elimination (dominated guards, one guard
for a block's accesses through one pointer, hoisting), proof-dependency
tracking for a sharper blame property, hardware tagging (Arm MTE, Apple
MIE), and the boundary facets of call sites.

## Done: RFC 0033 — Drop-in by default

Design: [RFC 0033 — Drop-in by default](rfcs/0033-drop-in-by-default.md)
(Implemented, with its build-cost gate D5 amended). A correct program built
with `CC=weavec-cc` and default flags compiles, links and runs as it does
with Clang, measured on eight fresh projects WeaveC was never tuned on and
on a sealed set built once at the end. The stages landed on the
`rfc0033-drop-in` branch and ship as one change; the decisions made while
implementing them are recorded in the RFC's *Implementation amendments*:

- [x] **S0 Tests first**: fresh and sealed configs, the `dropin/` cases.
- [x] **S1 Witnessed violations and raw pointers.**
- [x] **S2 The library table.**
- [x] **S3 Guards**: the accessed address, fortified calls, format arguments.
- [x] **S4 The runtime**: failure terminates, one runtime per process on
      Darwin, the system's heap in the arena.
- [x] **S5 The link step and records**, and the per-unit budget.
- [x] **S6 The driver and diagnostics.**
- [x] **S7 Fresh-corpus iterations.**
- [x] **S8 Gates, the sealed run, documentation.**

Every fresh project (zstd, libuv, oniguruma, redis, expat, pcre2,
libevent, libsodium) builds with its own build and passes its own tests
in trap and report mode with no trap and no false error. The sealed set
(libxml2, libpng, mbedtls, msgpack-c, yyjson), built once at the end,
found four more classes of false stops; they are fixed, and all five now
build and pass. One target was not met and is carried forward: the build
cost. libsodium, libevent, libuv and msgpack-c build within 4× the
reference compiler's CPU time, zstd and redis within 3.6–5.6×, and
oniguruma (6.1×), expat (8.1–8.7×), libxml2 (12–16×) and pcre2 (32–36×)
do not, because the analysis's joins rebuild the whole abstract state at
every merge. Making joins share what both sides hold is the next step for
build time.

## Next: RFC 0034 — Guard elimination

Dominated guards, the hull of a block's guards per pointer, loop hoisting
and guards as optimiser-visible intrinsics, against RFC 0032's carried cost
gate (Lua at most 2.0× and zlib at most 1.5× the reference compiler). The
build-cost gate RFC 0033 carries forward (every build within 4× the
reference compiler) needs joins that keep what both sides of a merge share
instead of renumbering the whole state; it is either part of this RFC or
the next.

## Planned: RFC 0035 — Adoption

- Format-31 records embedded in object sections, so archives, shared
  libraries, ccache and LTO carry them; incremental `-fweavec-link=analyze`.
- Fingerprinted baselines, reasoned suppressions and waivers for accepted
  definite errors.
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
