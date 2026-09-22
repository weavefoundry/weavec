/* rewrite-oracle-zero-init-address.c as the check emitter rewrites it. */
void *malloc(unsigned long);
void *(*allocate)(unsigned long) = __weavec_malloc_zero_fn;
void *make(unsigned long n) {
  void *(*f)(unsigned long) = __weavec_malloc_zero_fn;
  return ((void *(*)(unsigned long))__weavec_chk_nonnull_fn((void (*)(void))f))(n);
}
