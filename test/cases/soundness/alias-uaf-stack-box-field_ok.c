// RFC 0031 Motivation, correct twin of alias-uaf-stack-box-field_bug.c (probe p4 b2): the
// field is read through the alias before 'o' is freed.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
struct box { struct n *a; };
int b2(void) {
  struct box bx;
  struct n *o = malloc(sizeof *o);
  if (!o) abort();
  o->v = 7;
  bx.a = o;
  struct box *q = &bx;
  int r = q->a->v;
  free(o);
  return r;
}
int main(void) { return b2() == 7 ? 0 : 1; }
