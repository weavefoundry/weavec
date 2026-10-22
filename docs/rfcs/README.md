# WeaveC RFCs

Design changes to what WeaveC guards, how its runtime enforces it and what
it guarantees go through a lightweight RFC. The RFC is the durable record
of *why* the design is the way it is; the code and tests record *what* it
does.

## When an RFC is required

Write an RFC when a change would:

- change which accesses get a guard, or add or change a rule that removes
  one (`lib/Frontend/Guard*.cpp`);
- change the shadow encoding, frame layout, global redzones, the
  quarantine or the sweep;
- change the runtime's checked library wrappers, its report kinds or their
  formats, or the library table (`lib/Core/LibrarySpec.txt`);
- change the enforcement ledger's format or the driver's `-fweavec-*` flags;
- add or change an annotation in `resources/include/weavec.h`;
- change what WeaveC guarantees, including trading soundness for cost.

Do **not** write an RFC for driver plumbing, build or CI changes,
documentation, or bug fixes that bring the implementation in line with an
already-accepted RFC. Those go through normal
pull requests; release-worthy changes appear in the generated `CHANGELOG.md`.

## Process

1. Copy [`0000-template.md`](0000-template.md) to `NNNN-short-title.md` using
   the next unused number, fill it in, and open a pull request containing only
   the RFC. Discussion happens on that PR.
2. When there is consensus, the RFC is merged with status **Accepted**.
   Implementation proceeds in follow-up PRs that reference the RFC number.
3. When every item in the RFC's *Detailed design* and *Diagnostics* sections
   has landed with tests, a PR flips the status to **Implemented**. Lit tests
   that pin RFC behaviour are named by feature (`test/Guards/<feature>.c`,
   `test/cases/<area>/…`), and the pull request names the RFC.
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
| [0001](0001-ownership-model.md)          | Ownership, borrowing and lifetimes | Superseded |
| [0002](0002-intraprocedural-checking.md) | Sound intra-procedural checking    | Superseded |
| [0003](0003-signature-inference.md)      | Signature inference                | Superseded |
| [0004](0004-unsafe-boundaries.md)        | Unsafe boundaries: raw pointers, unsafe regions and indirect calls | Superseded |
| [0005](0005-whole-program-analysis.md)   | Whole-program analysis: cross-TU summaries and the compiler driver | Superseded |
| [0006](0006-precision.md)                | Precision: non-lexical loans, condition facts, element places and outcome-conditional summaries | Superseded |
| [0007](0007-resource-lifecycle.md)       | Resource lifecycle: leaks, release families and owned fields | Superseded |
| [0008](0008-pointer-validity.md)         | Pointer validity: null dereferences, uninitialised pointers, invalid releases and replaced values | Superseded |
| [0009](0009-value-conditional-behaviour.md) | Value-conditional behaviour: scalar facts, guarded effects and inferred `noreturn` | Superseded |
| [0010](0010-shared-ownership.md)         | Shared ownership: reference counts, ownership by outcome and per-outcome facts | Superseded |
| [0011](0011-spatial-safety.md)           | Spatial safety: derived pointers, extents and bounds | Superseded |
| [0012](0012-spatial-safety-strings-and-fields.md) | Spatial safety II: strings, sized fields, offset relations and assumptions | Superseded |
| [0013](0013-interprocedural-heap-state.md) | Interprocedural heap state and value identity | Superseded |
| [0014](0014-pointer-identity-and-call-effects.md) | Pointer identity and precise call effects | Superseded |
| [0015](0015-array-and-container-ownership.md) | Array elements, range operations, and container ownership | Superseded |
| [0016](0016-compositional-call-checking.md) | Compositional call checking under caller alias relationships | Superseded |
| [0017](0017-c-integer-semantics-and-spatial-safety.md) | C integer semantics and compositional spatial safety | Superseded |
| [0018](0018-checked-code-and-safety-contracts.md) | Compositional safety contracts and checked code | Superseded |
| [0019](0019-practical-checked-memory-contracts.md) | Practical checked memory contracts for buffers and heap objects | Superseded |
| [0020](0020-scalable-modular-checked-analysis.md) | Scalable modular checked analysis | Superseded |
| [0021](0021-practical-c-traversal.md) | Practical C traversal and inductive buffer contracts | Superseded |
| [0022](0022-checked-c-interfaces.md) | Compositional checked interfaces for opaque pointers and callbacks | Superseded |
| [0023](0023-inductive-container-contracts.md) | Inductive ownership contracts for linked containers | Superseded |
| [0024](0024-checked-runtime-contracts.md) | Checked C runtime contracts and variadic interfaces | Superseded |
| [0025](0025-case-sensitive-checked-contracts.md) | Case-sensitive checked contracts and discriminated C objects | Superseded |
| [0026](0026-growable-buffer-contracts.md) | Inferred relational contracts for growable buffers and vectors | Superseded |
| [0027](0027-recursive-object-ownership.md) | Recursive object ownership and complete cleanup contracts | Superseded |
| [0028](0028-opaque-objects-and-library-state.md) | Inferred contracts for opaque objects and private library state | Superseded |
| [0029](0029-compositional-recursive-workflows.md) | Compositional invariants for recursive C workflows | Superseded |
| [0030](0030-prove-or-trap.md) | Prove or trap: one safety semantics with compiler-enforced checks | Implemented (analysis superseded by [0036](0036-deterministic-heap-reuse.md)) |
| [0031](0031-object-engine.md) | The object engine: a sound heap abstraction behind the engine seam | Superseded |
| [0032](0032-runtime-enforcement.md) | Runtime enforcement: an object table, guarded facets and a temporal backstop | Implemented |
| [0033](0033-drop-in-by-default.md) | Drop-in by default: no false stops on code WeaveC was never tuned on | Implemented |
| [0034](0034-fast-enforcement.md) | Fast enforcement, confirmed errors and a bounded analysis | Implemented |
| [0035](0035-guard-by-default.md) | Guard by default: every access guarded in the backend, the analysis advisory | Implemented (superseded in part by [0036](0036-deterministic-heap-reuse.md)) |
| [0036](0036-deterministic-heap-reuse.md) | Deterministic heap reuse: reuse after a sweep, the analysis deleted | Accepted |

The [roadmap](../roadmap.md) links each milestone to the RFCs that define it.
