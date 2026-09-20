// RFC 0030, section 10.6, gate G8: the element at 0 of a sized parameter: nonnull innermost, then index.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-deref-sized.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-deref-sized.expected.c %t

#include <weavec.h>
char head(const char *WEAVEC_SIZED_BY(n) p, int n) { return *p; }
