/* rewrite-oracle-nonnull-conditional.c as the check emitter rewrites it. */
int pick(int c, int *p, int *q) { return *(int *)__weavec_chk_nonnull((c ? p : q)); }
