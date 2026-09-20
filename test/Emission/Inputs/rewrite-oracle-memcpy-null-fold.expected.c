/* rewrite-oracle-memcpy-null-fold.c as the check emitter rewrites it. */
void *memcpy(void *, const void *, unsigned long);
int fold(int *p, const int *x) {
  /* RFC 0030 §8.3: a zero length accepts null pointers: nothing to check. */
  memcpy(p, x, 0);
  return *(int *)__weavec_chk_nonnull(p);
}
