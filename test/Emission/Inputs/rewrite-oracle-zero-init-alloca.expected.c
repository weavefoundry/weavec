/* rewrite-oracle-zero-init-alloca.c as the check emitter rewrites it. */
int stack(unsigned long n) {
  char *p = __builtin_memset(__builtin_alloca(n), 0, n);
  /* RFC 0030 §8.2: alloca storage is never null and has `n` bytes. */
  return (__weavec_chk_index(0, n), p)[0];
}
