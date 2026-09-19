// RFC 0030, section 10.6, gate G8: volatile and const pointees keep their qualifiers through the check.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-qualifiers.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-qualifiers.expected.c %t

int vol(volatile int *p) { return *p; }
char at0(const char *s) { return s[0]; }
