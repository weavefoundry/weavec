// RFC 0031 Motivation, probe p3 a2 (build/rfc31/probes/p3.c): a stack cell 'cellv' holds 'o'
// through 'slot'; 'alias' copies 'slot'; 'o' is freed and read back through 'alias'.
// v0.11.0 proves the temporal facet of '(*alias)->v' (a false proof, ASan-confirmed): the
// fact that 'o' was freed is keyed to the paths known when it is made, not to the object.
// The line also dereferences 'alias' itself (the live cell, legitimately proven), so the
// marker cannot be a not-proven one (a line marker judges every row of the line): the BUG needs a
// diagnostic, and the ASan oracle (G4) needs the facet non-proven.
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a2(void) {             /* stack cell, alias */
  struct n *cellv;
  struct n *o = malloc(sizeof *o);
  if (!o) return 0;
  struct n **slot = &cellv;
  *slot = o;
  struct n **alias = slot;
  free(o);
  return (*alias)->v; // BUG: use-after-free
}
int main(void) { return a2() == 12345; }
