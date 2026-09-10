# WeaveC RFCs

Design changes to WeaveC's ownership model, checker rules or annotation
surface go through a lightweight RFC. The RFC is the durable record of *why*
the model is the way it is; the code and tests record *what* it does.

## When an RFC is required

Write an RFC when a change would:

- add to, remove from or change the meaning of anything in `weavec::Core`
  (the lattice, places, loans, lifetimes, move tracking);
- add a checker rule or change what an existing rule accepts or rejects
  (`lib/Analysis/Dataflow.cpp` and its successors);
- add or change an annotation in `resources/include/weavec.h`;
- add a diagnostic id or change the meaning of an existing one;
- change what WeaveC guarantees, including trading soundness for precision.

Do **not** write an RFC for driver plumbing, CLI flags, diagnostics
rendering, build or CI changes, documentation, or bug fixes that bring the
implementation in line with an already-accepted RFC. Those go through normal
pull requests; release-worthy changes appear in the generated `CHANGELOG.md`.

## Process

1. Copy [`0000-template.md`](0000-template.md) to `NNNN-short-title.md` using
   the next unused number, fill it in, and open a pull request containing only
   the RFC. Discussion happens on that PR.
2. When there is consensus, the RFC is merged with status **Accepted**.
   Implementation proceeds in follow-up PRs that reference the RFC number.
3. When every item in the RFC's *Detailed design* and *Diagnostics* sections
   has landed with tests, a PR flips the status to **Implemented**. Lit tests
   that pin RFC behaviour should carry the number in their filename
   (`test/Analysis/rfc0002-*.c`) so the mapping stays greppable.
4. An RFC that is abandoned is marked **Withdrawn**; one replaced by a later
   RFC is marked **Superseded** with a link. RFCs are never deleted or
   renumbered.

Statuses: `Draft` → `Accepted` → `Implemented`, or `Withdrawn` /
`Superseded`.

Small corrections to an Accepted RFC (typos, clarifying a sentence to match
what was actually built) may be made directly. Anything that changes a
decision is a new RFC that supersedes the relevant section.

## Index

| RFC                                      | Title                              | Status   |
| ---------------------------------------- | ---------------------------------- | -------- |
| [0001](0001-ownership-model.md)          | Ownership, borrowing and lifetimes | Accepted |
| [0002](0002-intraprocedural-checking.md) | Sound intra-procedural checking    | Implemented |
| [0003](0003-signature-inference.md)      | Signature inference                | Implemented |
| [0004](0004-unsafe-boundaries.md)        | Unsafe boundaries: raw pointers, unsafe regions and indirect calls | Implemented |
| [0005](0005-whole-program-analysis.md)   | Whole-program analysis: cross-TU summaries and the compiler driver | Implemented |
| [0006](0006-precision.md)                | Precision: non-lexical loans, condition facts, element places and outcome-conditional summaries | Implemented |
| [0007](0007-resource-lifecycle.md)       | Resource lifecycle: leaks, release families and owned fields | Accepted |
| [0008](0008-pointer-validity.md)         | Pointer validity: null dereferences, uninitialised pointers, invalid releases and replaced values | Accepted |
| [0009](0009-value-conditional-behaviour.md) | Value-conditional behaviour: scalar facts, guarded effects and inferred `noreturn` | Accepted |
| [0010](0010-shared-ownership.md)         | Shared ownership: reference counts, ownership by outcome and per-outcome facts | Accepted |
| [0011](0011-spatial-safety.md)           | Spatial safety: derived pointers, extents and bounds | Accepted |
| [0012](0012-spatial-safety-strings-and-fields.md) | Spatial safety II: strings, sized fields, offset relations and assumptions | Accepted |
| [0013](0013-interprocedural-heap-state.md) | Interprocedural heap state and value identity | Accepted |
| [0014](0014-pointer-identity-and-call-effects.md) | Pointer identity and precise call effects | Implemented |
| [0015](0015-array-and-container-ownership.md) | Array elements, range operations, and container ownership | Implemented |
| [0016](0016-compositional-call-checking.md) | Compositional call checking under caller alias relationships | Implemented |
| [0017](0017-c-integer-semantics-and-spatial-safety.md) | C integer semantics and compositional spatial safety | Implemented |
| [0018](0018-checked-code-and-safety-contracts.md) | Compositional safety contracts and checked code | Accepted |
| [0019](0019-practical-checked-memory-contracts.md) | Practical checked memory contracts for buffers and heap objects | Implemented |
| [0020](0020-scalable-modular-checked-analysis.md) | Scalable modular checked analysis | Implemented |
| [0021](0021-practical-c-traversal.md) | Practical C traversal and inductive buffer contracts | Implemented |

The [roadmap](../roadmap.md) links each milestone to the RFCs that define it.
