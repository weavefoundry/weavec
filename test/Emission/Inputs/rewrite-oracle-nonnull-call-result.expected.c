/* rewrite-oracle-nonnull-call-result.c as the check emitter rewrites it. */
struct node { int value; struct node *next; };
struct node *next(struct node *n);
int after(struct node *n) { return ((struct node *)__weavec_chk_nonnull(next(n)))->value; }
