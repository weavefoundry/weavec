/* rewrite-oracle-verify-proven.c as the check emitter rewrites it in verify
 * mode: a proven facet takes the same check as a checked one, from the
 * __weavec_prv_ family (§10.7). The subscript is checked against the array's
 * exact extent, 4 elements; the dereference wraps its pointer in place. */
int pick(void) {
  int a[4] = {0, 1, 2, 3};
  return a[__weavec_prv_index(2, 4)];
}

int deref(void) {
  int x = 5;
  int *p = &x;
  return *(int *)__weavec_prv_nonnull(p);
}
