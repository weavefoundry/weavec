/* rewrite-oracle-nonnull-compound-assign.c as the check emitter rewrites it. */
struct counter { int n; };
void bump(int *p, struct counter *s, int *q) {
  *(int *)__weavec_chk_nonnull(p) += 2;
  ((struct counter *)__weavec_chk_nonnull(s))->n *= 3;
  ((int *)__weavec_chk_nonnull(q))[0] -= 1;
}
