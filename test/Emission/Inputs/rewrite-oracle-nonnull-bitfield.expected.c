/* rewrite-oracle-nonnull-bitfield.c as the check emitter rewrites it. */
struct flags { unsigned a : 3; unsigned b : 5; };
unsigned getb(struct flags *f) { return ((struct flags *)__weavec_chk_nonnull(f))->b; }
void setb(struct flags *f) { ((struct flags *)__weavec_chk_nonnull(f))->b = 7; }
