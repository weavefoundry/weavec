// RFC 0031 Motivation, correct twin of alias-uaf-stack-cell_bug.c (probe p3 a2): the value
// is read through the alias of the stack cell before 'o' is freed.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a2(void) {
  struct n *cellv;
  struct n *o = malloc(sizeof *o);
  if (!o) return 0;
  o->v = 7;
  struct n **slot = &cellv;
  *slot = o;
  struct n **alias = slot;
  int r = (*alias)->v;
  free(o);
  return r;
}
int main(void) { return a2() == 7 ? 0 : 1; }
