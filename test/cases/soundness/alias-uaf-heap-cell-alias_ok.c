// RFC 0031 Motivation, correct twin of alias-uaf-heap-cell-alias_bug.c (probe p3 a3): the
// value is read through the alias before 'o' is freed, and the cell is freed.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a3(void) {
  struct n **slot = malloc(sizeof *slot);
  struct n *o = malloc(sizeof *o);
  if (!slot) abort();
  if (!o) abort();
  o->v = 7;
  *slot = o;
  struct n **alias = slot;
  int r = (*alias)->v;
  free(o);
  free(slot);
  return r;
}
int main(void) { return a3() == 7 ? 0 : 1; }
