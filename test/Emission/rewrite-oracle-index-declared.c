// RFC 0030, section 10.6, gate G8: a parameter sized by another: the signed count enters through have_s.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-declared.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-declared.expected.c %t

#include <weavec.h>
char at(const char *WEAVEC_SIZED_BY(n) p, int n, int i) { return p[i]; }
