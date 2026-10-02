// RFC 0031 Motivation, correct twin of alias-uaf-heap-cell_bug.c (probe p3 a1): the value is
// read through the heap cell before 'o' is freed.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a1(void) {
  struct n **slot = malloc(sizeof *slot);
  struct n *o = malloc(sizeof *o);
  if (!slot || !o) { free(slot); free(o); return 0; }
  o->v = 7;
  *slot = o;
  int r = (*slot)->v;
  free(o);
  free(slot);
  return r;
}
int main(void) { return a1() == 7 ? 0 : 1; }
