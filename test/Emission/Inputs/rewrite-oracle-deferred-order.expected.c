/* rewrite-oracle-deferred-order.c as the check emitter rewrites it. */
static int later(int *p);
int early(int *p) { return later((int *)__weavec_chk_nonnull(p)); }
static int later(int *p) { return *p + 1; }
static inline int helper(int *p) { return *p; }
int use(int *p) { return helper((int *)__weavec_chk_nonnull(p)); }
