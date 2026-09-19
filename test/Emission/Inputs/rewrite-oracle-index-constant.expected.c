/* rewrite-oracle-index-constant.c as the check emitter rewrites it. */
void *malloc(unsigned long);
char *third(unsigned long n) {
  char *a = __weavec_malloc_zero(n);
  ((char *)__weavec_chk_nonnull(a))[__weavec_chk_index(2, n)] = 1;
  return a;
}
