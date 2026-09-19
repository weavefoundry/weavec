/* rewrite-oracle-nonnull-subscript-zero.c as the check emitter rewrites it. */
int first(const int *p) { return ((const int *)__weavec_chk_nonnull(p))[0]; }
