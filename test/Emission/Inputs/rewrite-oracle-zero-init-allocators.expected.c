/* rewrite-oracle-zero-init-allocators.c as the check emitter rewrites it. */
void *malloc(unsigned long);
void *calloc(unsigned long, unsigned long);
void *realloc(void *, unsigned long);
void free(void *);
void *make(unsigned long n) {
  void *p = __weavec_malloc_zero(n);
  void *q = __weavec_calloc_zero(n, 4);
  p = __weavec_realloc_zero(p, 2 * n);
  free(q);
  return p;
}
