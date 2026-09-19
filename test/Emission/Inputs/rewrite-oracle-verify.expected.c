/* rewrite-oracle-verify.c as the check emitter rewrites it. */
int load(int *p, int i) {
  int a[4] = {0};
  return *(int *)__weavec_chk_nonnull(p) + a[__weavec_chk_index(i, 4)];
}
