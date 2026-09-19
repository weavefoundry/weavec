// RFC 0030, section 10.6, gate G8: stores, ++, compound assignment and *(a + i) on a checked subscript.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-lvalues.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-lvalues.expected.c %t

int fill(int i, int v) {
  int a[8] = {0};
  a[i] = v;
  a[i]++;
  a[i] += v;
  *(a + i) = v;
  return a[0];
}
