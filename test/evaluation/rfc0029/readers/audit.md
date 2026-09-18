# Initial reader probe audit

The initial manifest is retained unchanged, including an invalid negative:
`reader-escape` starts beyond the extent but calls `take`, whose guard returns
without accessing data. That program is safe and cannot support a false-proof
claim. It is excluded from the corrected correctness population; the replacement
must actually access the escaped cursor. Its initial failed expectation remains
recorded in the development results.

Both the immutable HEAD baseline and candidate 15 wrongly accept `reader-short`
and `reader-uninitialized`: `consume` accesses every position through `end`, but
its exported contract only requires the cell at the incoming `position`.
The affine projection used an entry path even after a numeric write. This is a
pre-existing soundness bug, independently discovered after the main frozen
population and full candidate-15 Debug/sanitizer validation.
