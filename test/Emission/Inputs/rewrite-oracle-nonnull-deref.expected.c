/* rewrite-oracle-nonnull-deref.c as the check emitter rewrites it. */
int load(int *p) { return *(int *)__weavec_chk_nonnull(p); }
void store(int *p, int v) { *(int *)__weavec_chk_nonnull(p) = v; }
