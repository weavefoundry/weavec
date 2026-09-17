# Recursive argument offsets

Frozen after candidate 17 and before the offset-origin correction. A proof
boundary review found that resolvePointerValue intentionally strips arithmetic
for ordinary ownership identity. It cannot establish the actual argument of an
induction hypothesis. Candidate 17 incorrectly accepts forward.c. The negative
cases require exact-node/proper-child evidence; forming an allowed one-past
pointer cannot justify applying a node cleanup contract to it. zero.c preserves
an equivalent zero-offset spelling.
