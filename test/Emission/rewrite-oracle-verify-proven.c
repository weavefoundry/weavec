// RFC 0030, sections 10.6 and 10.7, gate G8: verify mode is the soundness
// monitor, so it checks a *proven* facet too, with the __weavec_prv_ family
// (trap category weavec.proven). Both facets here are proven: the subscript
// against the array's exact extent, and the pointer of the dereference.
// The -O0 IR equals that of Inputs/rewrite-oracle-verify-proven.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-verify-proven.expected.c %t -- -fweavec-checks=verify

int pick(void) {
  int a[4] = {0, 1, 2, 3};
  return a[2];
}

int deref(void) {
  int x = 5;
  int *p = &x;
  return *p;
}
