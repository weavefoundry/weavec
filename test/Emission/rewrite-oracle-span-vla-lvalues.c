// RFC 0030, section 10.6, gate G8: stores, ++, compound assignment and *(v + i) through the span form.
// The -O0 IR equals that of Inputs/rewrite-oracle-span-vla-lvalues.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-span-vla-lvalues.expected.c %t
//
// RFC 0031 *Implementation amendments*, "Variable-length arrays": a
// dimension that may be zero or negative gives no storage to check an access
// against, so the test of `n` is what gives `v` its extent here. Without it
// the accesses are unresolved(unknown-extent) and get no check
// (rewrite-oracle-span-vla.c; test/cases/KNOWN-DIFFERENCES.md, *Lit tests*).

long put(int n, int i, long x) {
  if (n < 1)
    return 0;
  long v[n];
  v[i] = x;
  v[i]++;
  v[i] += x;
  *(v + i) = x;
  return x;
}
