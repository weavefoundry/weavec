// RFC 0030, section 10.6, gate G8: stores, ++, compound assignment and *(v + i) through the span form.
// The -O0 IR equals that of Inputs/rewrite-oracle-span-vla-lvalues.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-span-vla-lvalues.expected.c %t

long put(int n, int i, long x) {
  long v[n];
  v[i] = x;
  v[i]++;
  v[i] += x;
  *(v + i) = x;
  return x;
}
