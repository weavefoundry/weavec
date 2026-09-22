/* rewrite-oracle-nonnull-statement-expression.c as the check emitter rewrites it. */
int stmt(int *p) { return ({ int v = *(int *)__weavec_chk_nonnull(p); v + 1; }); }
int gen(int *p) { return _Generic(p, int *: *(int *)__weavec_chk_nonnull(p), default: 0); }
