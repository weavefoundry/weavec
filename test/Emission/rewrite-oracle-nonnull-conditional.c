// RFC 0030, section 10.6, gate G8: a conditional pointer is evaluated once, inside the check.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-conditional.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-conditional.expected.c %t

int pick(int c, int *p, int *q) { return *(c ? p : q); }
