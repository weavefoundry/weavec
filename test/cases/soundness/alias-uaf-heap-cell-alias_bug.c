// RFC 0031 Motivation, probe p3 a3 (build/rfc31/probes/p3.c): a heap cell holds 'o', 'alias'
// copies the cell's address after the store, 'o' is freed and read back through 'alias'.
// v0.11.0 proves the temporal facet of '(*alias)->v' (a false proof, ASan-confirmed).
// The cell is deliberately not freed (the probe's shape); only the leak may be reported
// besides the bug. The line also dereferences 'alias' itself (legitimately proven), so the
// bug marker needs a diagnostic; see alias-uaf-stack-cell_bug.c.
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a3(void) {             /* heap cell, alias, no early-return join */
  struct n **slot = malloc(sizeof *slot);
  struct n *o = malloc(sizeof *o);
  if (!slot) abort();
  if (!o) abort();
  *slot = o;
  struct n **alias = slot;
  free(o);
  return (*alias)->v; // BUG: use-after-free
}
int main(void) { return a3() == 12345; }
