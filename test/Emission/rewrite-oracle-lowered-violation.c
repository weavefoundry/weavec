// RFC 0030, sections 3.4 and 10.6, gate G8: a definite violation whose
// error -Wno-error lowers to a warning still traps. A temporal violation has
// no check of its own, so the second `free` is preceded by the unconditional
// trap: `(__weavec_chk_violation(), free(p))`.
// The -O0 IR equals that of Inputs/rewrite-oracle-lowered-violation.expected.c
// compiled by the reference Clang with the printed prelude.
//
// XFAIL: *
// The engine of this stage publishes only the default outcomes (S3-A), so
// the double free is diagnosed but its facet is not a violation, and nothing
// guards it; the planner (Form::Violation) and the emitter are in place
// (unittests/Frontend/CheckEmitterTest.cpp, LoweredViolationOracle). Remove
// the XFAIL when the engine publishes violations.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-lowered-violation.expected.c %t -- -Wno-error=weavec-double-free

void free(void *);
void twice(int *p) {
  free(p);
  free(p);
}
