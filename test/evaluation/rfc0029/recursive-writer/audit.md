# Frozen writer population audit

The original candidate24 inventory and baseline are retained unchanged. Two
expectations need a more precise interpretation before writer implementation:

- `cycle.c` is memory safe: its output-capacity guard eventually returns zero,
  even though the input interval never decreases. Candidate24 accepts it with
  an ordinary conditional memory contract. It is a conservative nondecreasing
  input-induction probe, not a demonstrated memory-safety counterexample.
- `false-success.c` returns success without finishing the input, but its client
  reads no output. That standalone program is memory safe. A buffer invariant
  alone does not promise complete copying, so accepting it is not a false proof.

The separately frozen `recursive-writer-reviewed` population retains the five
well-founded original cases and adds clients that inspect the initialized
prefix, a genuinely false advertised length, partial failure, aliasing and
mutual forwarding. Original expectations are neither overwritten nor counted
as passing measurements for the reviewed population. The stronger complete-
serialization obligation still belongs to RFC 0029's required workflow gates.
