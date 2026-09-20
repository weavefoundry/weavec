// RFC 0030, section 10.6, gate G8: the base of `->` is checked; the member access stays an lvalue.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-arrow.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-arrow.expected.c %t

struct point { int x, y; };
int gety(struct point *p) { return p->y; }
void setx(struct point *p, int v) { p->x = v; }
