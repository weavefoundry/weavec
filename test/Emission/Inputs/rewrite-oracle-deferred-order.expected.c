/* rewrite-oracle-deferred-order.c as the check emitter rewrites it. */
static int later(int *p);
int early(int *p) { return later(p); }
static int later(int *p) { return *(int *)__weavec_chk_nonnull(p) + 1; }
static inline int helper(int *p) { return *(int *)__weavec_chk_nonnull(p); }
int use(int *p) { return helper(p); }
