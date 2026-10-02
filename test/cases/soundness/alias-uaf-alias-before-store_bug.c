// RFC 0031 Motivation, probe p3 a4 (build/rfc31/probes/p3.c): 'alias' copies the heap cell's
// address before 'o' is stored into it; 'o' is freed and read back through 'alias'.
// v0.11.0 reports it (definite use-after-free). The line also dereferences 'alias' itself
// (legitimately proven), so the bug marker needs a diagnostic; see alias-uaf-stack-cell_bug.c.
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int a4(void) {             /* heap cell, alias assigned before store */
  struct n **slot = malloc(sizeof *slot);
  struct n *o = malloc(sizeof *o);
  if (!slot) abort();
  if (!o) abort();
  struct n **alias = slot;
  *slot = o;
  free(o);
  return (*alias)->v; // BUG: use-after-free
}
int main(void) { return a4() == 12345; }
