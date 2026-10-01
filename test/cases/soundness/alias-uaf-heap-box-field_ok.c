// RFC 0031 Motivation, correct twin of alias-uaf-heap-box-field_bug.c (probe p4 b1): the
// field is read through the alias before 'o' is freed.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
struct box { struct n *a; };
int b1(void) {
  struct box *bp = malloc(sizeof *bp);
  struct n *o = malloc(sizeof *o);
  if (!bp || !o) abort();
  o->v = 7;
  bp->a = o;
  struct box *q = bp;
  int r = q->a->v;
  free(o);
  free(bp);
  return r;
}
int main(void) { return b1() == 7 ? 0 : 1; }
