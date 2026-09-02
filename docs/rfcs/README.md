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
pull requests and `CHANGELOG.md`.

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

The [roadmap](../roadmap.md) links each milestone to the RFCs that define it.
