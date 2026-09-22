/* rewrite-oracle-nonnull-struct-value.c as the check emitter rewrites it. */
struct big { int v[4]; };
struct big get(struct big *p) { return *(struct big *)__weavec_chk_nonnull(p); }
void put(struct big *p, struct big b) { *(struct big *)__weavec_chk_nonnull(p) = b; }
