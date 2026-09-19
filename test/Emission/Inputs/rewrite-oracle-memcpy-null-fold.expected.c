/* rewrite-oracle-memcpy-null-fold.c as the check emitter rewrites it. */
void *memcpy(void *, const void *, unsigned long);
int fold(int *p, const int *x) {
  memcpy(__weavec_chk_nonnull_n(p, 0), __weavec_chk_nonnull_n(x, 0), 0);
  return *(int *)__weavec_chk_nonnull(p);
}
