// RFC 0031 Motivation, probe p3 a1 (build/rfc31/probes/p3.c): the control without an alias.
// A heap cell holds the only pointer to 'o'; 'o' is freed and read back through the cell.
// v0.11.0 reports it (definite use-after-free); kept as the control of the alias probes.
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a1(void) {             /* no alias, heap cell */
  struct n **slot = malloc(sizeof *slot);
  struct n *o = malloc(sizeof *o);
  if (!slot || !o) { free(slot); free(o); return 0; }
  *slot = o;
  free(o);
  int r = (*slot)->v; // BUG: use-after-free
  free(slot);
  return r;
}
int main(void) { return a1() == 12345; }
