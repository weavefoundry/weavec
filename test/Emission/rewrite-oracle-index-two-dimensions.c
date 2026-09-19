// RFC 0030, section 10.6, gate G8: each subscript of a two-dimensional array against its own bound.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-two-dimensions.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-two-dimensions.expected.c %t

int cell(int i, int j) {
  int m[3][4] = {{0}};
  return m[i][j];
}
