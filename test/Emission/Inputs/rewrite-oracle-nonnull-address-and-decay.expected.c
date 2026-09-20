/* rewrite-oracle-nonnull-address-and-decay.c as the check emitter rewrites it. */
struct record { int n; char name[8]; };
int *field(struct record *r) { return &((struct record *)__weavec_chk_nonnull(r))->n; }
char *name(struct record *r) { return ((struct record *)__weavec_chk_nonnull(r))->name; }
