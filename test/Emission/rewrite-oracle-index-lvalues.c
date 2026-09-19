// RFC 0030, section 10.6, gate G8: stores, ++, compound assignment and *(a + i) on a checked subscript.
// Each context uses its own index: a planned index check may bound its index
// downstream (section 3.2), which would prove a second subscript with it.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-lvalues.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-lvalues.expected.c %t

int fill(int i, int j, int k, int m, int v) {
  int a[8] = {0};
  a[i] = v;
  a[j]++;
  a[k] += v;
  *(a + m) = v;
  return v;
}
