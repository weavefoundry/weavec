/* rewrite-oracle-nonnull-qualifiers.c as the check emitter rewrites it. */
int vol(volatile int *p) { return *(volatile int *)__weavec_chk_nonnull(p); }
char at0(const char *s) { return ((const char *)__weavec_chk_nonnull(s))[0]; }
