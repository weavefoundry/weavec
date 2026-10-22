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
What problem does this solve? What does WeaveC get wrong, miss, or fail to
build or run today? Who is affected (which kind of C code, which users)? What is
the cost of not doing this?
-->

## Soundness

<!--
WeaveC's value is the guarantee it gives, so this section is mandatory for
anything touching the guards, the rules that remove them or the runtime. Be explicit about:

- **Bugs caught.** The class of memory-safety errors this proposal detects,
  ideally as small C programs that must trap.
- **Bugs deliberately not caught.** What remains outside the guarantee and
  why (out of scope, needs a later RFC, requires `WEAVEC_UNSAFE`).
- **Accepted false traps.** Correct code this proposal will stop, with
  the reasoning for why that trade-off is acceptable and what the user does
  about it (restructure, mark unsafe).
- **Assumptions.** Anything the guarantee depends on (no data races between
  a guard and its access, pointers kept visible to the sweep, ...).

If a proposal makes WeaveC *less* sound in some case in exchange for
lower cost or fewer false traps, say so here in plain terms.
-->

## Detailed design

<!--
Enough detail that someone familiar with the codebase could implement it
without re-deriving the decisions. Cover, as applicable:

- Changes to the guard passes (`lib/Frontend/Guard*.cpp`): which accesses
  get a guard, which rules remove one (local to a function, and monitored
  by `verify` mode), what the inline check does.
- Changes to the runtime (`runtime/`): the shadow, the arena, frame blocks,
  the quarantine and the sweep, the checked library wrappers, the reports.
- Changes to the library table (`lib/Core/LibrarySpec.txt`), the
  enforcement ledger or the `weavec-cc` driver, if any.
- Interaction with existing RFCs; anything they say that stops being true.
- Performance expectations (run time, memory, build time) and how they will
  be measured.
-->

## Annotation surface

<!--
Required if the proposal adds, removes or changes the meaning of anything in
`resources/include/weavec.h`. Give the macro spellings, what they attach to,
what they expand to under non-WeaveC compilers, and what they change in the
build. Mirror the change in `lib/Frontend/UnsafeRegions.cpp` and
`docs/annotations.md`.

Write "None." if the user-facing header is untouched.
-->

## Diagnostics

<!--
Required if the proposal adds or changes a compiler diagnostic or a
run-time report kind. For each report kind:

- its name (kebab-case, e.g. `heap-use-after-free`) and the report lines,
  with placeholders in angle brackets, e.g.
  `weavec: <kind> at <file>:<line>:<col>: read of <n> bytes at <address>`;
- a minimal C program that triggers it.

Every kind listed here needs a `test/Guards/` test pinning its report lines
and an entry in the command-line reference's list of kinds before the RFC
can move to Implemented.

Write "None." if nothing changes.
-->

## Drawbacks

<!--
Why might we not do this? Complexity added to the passes or the runtime,
run-time, memory or build cost, maintenance burden, risk of false traps on
code that builds and passes today.
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

- AddressSanitizer, HWASan and Arm MTE: shadow memory, redzones, tags.
- Low-fat pointers, SoftBound and CHERI: bounds from provenance.
- MarkUs, MineSweeper and the Boehm–Demers–Weiser collector: reuse after
  a conservative scan.
- `_FORTIFY_SOURCE`, Checked C, Cyclone and other safe-C dialects.

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
