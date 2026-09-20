/* rewrite-oracle-nonnull-arrow.c as the check emitter rewrites it. */
struct point { int x, y; };
int gety(struct point *p) { return ((struct point *)__weavec_chk_nonnull(p))->y; }
void setx(struct point *p, int v) { ((struct point *)__weavec_chk_nonnull(p))->x = v; }
