# RFC 0000: <Title>

- **Status**: Draft
- **Authors**: <name(s)>
- **Created**: <YYYY-MM-DD>
- **Tracking issue**: <link or "TBD">
- **Supersedes / superseded by**: <RFC links, or "—">

<!--
Copy this file to `docs/rfcs/NNNN-short-title.md`, where NNNN is the next
unused number. Delete the HTML comments as you fill in each section. Sections
that genuinely do not apply may be shortened to a one-line justification; do
not delete them, so readers can see the question was considered.
-->

## Summary

<!-- One paragraph. What changes, for whom, and what it makes possible. -->

## Motivation

<!--
What problem does this solve? What does WeaveC get wrong, miss, or refuse to
analyse today? Who is affected (which kind of C code, which users)? What is
the cost of not doing this?
-->

## Soundness

<!--
WeaveC's value is the guarantee it gives, so this section is mandatory for
anything touching the model or the checker. Be explicit about:

- **Bugs caught.** The class of memory-safety errors this proposal detects,
  ideally as small C examples that must be rejected.
- **Bugs deliberately not caught.** What remains outside the guarantee and
  why (out of scope, needs a later RFC, requires `WEAVEC_UNSAFE`).
- **Accepted false positives.** Sound code this proposal will reject, with
  the reasoning for why that trade-off is acceptable and what the user does
  about it (restructure, annotate, mark unsafe).
- **Assumptions.** Anything the guarantee depends on (no data races, no
  `longjmp`, callee summaries are trusted, ...).

If a proposal makes the checker *less* sound in some case in exchange for
fewer false positives, say so here in plain terms.
-->

## Detailed design

<!--
Enough detail that someone familiar with the codebase could implement it
without re-deriving the decisions. Cover, as applicable:

- Changes to `weavec::Core` (new facts, lattice changes, new state carried
  through the dataflow). Remember Core may not depend on Clang/LLVM.
- Changes to `weavec::Analysis` (which AST nodes / CFG elements produce which
  core events; new recognised functions; new options).
- Changes to `weavec::Frontend` or the `weavec` driver, if any.
- Interaction with existing RFCs; anything they say that stops being true.
- Performance expectations (per-function cost, per-TU cost) and how they will
  be measured.
-->

## Annotation surface

<!--
Required if the proposal adds, removes or changes the meaning of anything in
`resources/include/weavec.h`. Give the macro spellings, what they attach to,
what they expand to under non-WeaveC compilers, and how they interact with
inference (authoritative vs. hint). Mirror the change in
`include/weavec/Analysis/Annotations.h` and `docs/annotations.md`.

Write "None." if the user-facing header is untouched.
-->

## Diagnostics

<!--
Required if the proposal adds or changes diagnostics. For each one:

- the stable id to add to `weavec::core::diag` (kebab-case, e.g.
  `lifetime-too-short`) and its severity;
- the primary message and the notes attached to it, with placeholders in
  angle brackets, e.g. `use of '<p>' after it was freed`;
- a minimal C snippet that triggers it.

Every id listed here needs a unit test, a lit test pinning the message, and
a row in `docs/annotations.md` before the RFC can move to Implemented.

Write "None." if no diagnostics change.
-->

## Drawbacks

<!--
Why might we not do this? Complexity added to Core, analysis-time cost,
maintenance burden, risk of regressions in the false-positive rate,
compatibility with code already annotated under earlier RFCs.
-->

## Alternatives

<!--
Other designs considered and what each gives up. Include "do nothing" if it
is a real option. Explain why the chosen design is preferred.
-->

## Prior art

<!--
WeaveC has no reference implementation to match, so this section is where we
borrow judgement. Cite specifics (paper section, source file, RFC number)
from, as relevant:

- Rust: the borrow checker, NLL (RFC 2094), Polonius, `Box`/`&`/`&mut`.
- Cyclone, Checked C, and other safe-C dialects.
- Clang: `-Wdangling`, `[[clang::lifetimebound]]`, the lifetime-safety
  analysis, the static analyzer's `MallocChecker`.
- The C++ Core Guidelines lifetime profile and GSL `owner<T>`.
- Classic static analyses (escape analysis, region inference, typestate).

Say what each got right, what it got wrong for C, and what we take from it.
-->

## Unresolved questions

<!--
What must be answered before this RFC is Accepted? What can be deferred to
implementation? What do we expect to learn only from running against real
code? Keep this list honest; it is the most useful section for whoever picks
the work up next.
-->

## Future work

<!--
Follow-on changes this enables but that are out of scope here. Prefer to
name the RFC that should own each item if one is planned.
-->
