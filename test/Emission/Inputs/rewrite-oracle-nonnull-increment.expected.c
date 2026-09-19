/* rewrite-oracle-nonnull-increment.c as the check emitter rewrites it. */
struct counter { int n; };
void step(int *p, struct counter *s) {
  (*(int *)__weavec_chk_nonnull(p))++;
  ++((struct counter *)__weavec_chk_nonnull(s))->n;
  ((struct counter *)__weavec_chk_nonnull(s))->n--;
  --*(int *)__weavec_chk_nonnull(p);
}
