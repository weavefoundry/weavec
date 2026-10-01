// RFC 0031 Motivation, correct twin of alias-uaf-alias-before-store_bug.c (probe p3 a4): the
// value is read through the alias before 'o' is freed, and the cell is freed.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a4(void) {
  struct n **slot = malloc(sizeof *slot);
  struct n *o = malloc(sizeof *o);
  if (!slot) abort();
  if (!o) abort();
  o->v = 7;
  struct n **alias = slot;
  *slot = o;
  int r = (*alias)->v;
  free(o);
  free(slot);
  return r;
}
int main(void) { return a4() == 7 ? 0 : 1; }
