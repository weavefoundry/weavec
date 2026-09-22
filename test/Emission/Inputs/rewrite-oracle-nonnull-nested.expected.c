/* rewrite-oracle-nonnull-nested.c as the check emitter rewrites it. */
struct node { int value; struct node *next; };
int second(struct node *n) {
  return ((struct node *)__weavec_chk_nonnull(((struct node *)__weavec_chk_nonnull(n))->next))->value;
}
int twice(int **pp) { return *(int *)__weavec_chk_nonnull(*(int **)__weavec_chk_nonnull(pp)); }
