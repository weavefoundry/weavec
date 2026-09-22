// RFC 0030, section 10.6, gate G8: `p[0]` is a dereference: its base is checked.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-subscript-zero.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-subscript-zero.expected.c %t

int first(const int *p) { return p[0]; }
