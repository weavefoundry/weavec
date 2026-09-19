/* rewrite-oracle-nonnull-zero-length.c as the check emitter rewrites it. */
void *memcpy(void *, const void *, unsigned long);
void copy(char *d, const char *s, unsigned long n) {
  memcpy(__weavec_chk_nonnull_n(d, n), __weavec_chk_nonnull_n(s, n), n);
}
