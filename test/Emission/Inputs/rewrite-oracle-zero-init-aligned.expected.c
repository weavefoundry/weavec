/* rewrite-oracle-zero-init-aligned.c as the check emitter rewrites it. */
void *aligned_alloc(unsigned long, unsigned long);
void *get(unsigned long n) { return __weavec_zero_tail(aligned_alloc(16, n), 0); }
