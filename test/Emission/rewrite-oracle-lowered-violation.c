// RFC 0034 §6.4, gate G8: a definite violation whose error -Wno-error lowers
// to a warning gets the check or guard its facet would have as a possible
// finding, never an unconditional trap. With the runtime, the second `free`
// is guarded like any release (`__weavec_chk_release`), which traps only
// when the block is not live; without it, nothing is inserted and the facet
// is `unresolved(lowered)`.
// The -O0 IR equals that of the Inputs/rewrite-oracle-lowered-violation*.expected.c
// files compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-lowered-violation.expected.c %t -- -Wno-error=weavec-double-free -fweavec-runtime
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-lowered-violation-plain.expected.c %t.plain -- -Wno-error=weavec-double-free -fno-weavec-runtime

void free(void *);
void twice(int *p) {
  free(p);
  free(p);
}
