/* rewrite-oracle-zero-init-alloca.c as the check emitter rewrites it. */
int stack(unsigned long n) {
  char *p = __builtin_memset(__builtin_alloca(n), 0, n);
  return ((char *)__weavec_chk_nonnull(p))[0];
}
