// RFC 0030, section 10.6, gate G8: WEAVEC_ASSUME(e) becomes an assert check of e (ReplaceCall).
// The -O0 IR equals that of Inputs/rewrite-oracle-assume.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-assume.expected.c %t

#include <weavec.h>
int positive(int n) {
  WEAVEC_ASSUME(n > 0);
  return n;
}
