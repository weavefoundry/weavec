// RFC 0030, section 10.6, gate G8: a constant subscript is checked too (the optimiser folds it).
// The -O0 IR equals that of Inputs/rewrite-oracle-index-constant.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-constant.expected.c %t

int third(void) {
  int a[4] = {1, 2, 3, 4};
  return a[2];
}
