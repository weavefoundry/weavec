// RFC 0030, section 10.6, gate G8: a library argument that must not be null (WrapArgument).
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-argument.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-argument.expected.c %t

unsigned long strlen(const char *);
unsigned long len(const char *s) { return strlen(s); }
