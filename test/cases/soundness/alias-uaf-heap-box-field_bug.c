// RFC 0031 Motivation, probe p4 b1 (build/rfc31/probes/p4.c): a heap 'box' holds 'o' in its
// field 'a'; 'q' copies 'bp'; 'o' is freed and read back as 'q->a->v'.
// v0.11.0 proves the temporal facet of 'q->a->v' (a false proof, ASan-confirmed). The line
// also loads 'q->a' from the live box (legitimately proven), so the bug marker needs a diagnostic;
// see alias-uaf-stack-cell_bug.c.
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
struct box { struct n *a; };
int b1(void) {
  struct box *bp = malloc(sizeof *bp);
  struct n *o = malloc(sizeof *o);
  if (!bp || !o) abort();
  bp->a = o;
  struct box *q = bp;
  free(o);
  int r = q->a->v; // BUG: use-after-free
  free(bp);
  return r;
}
int main(void) { return b1() == 12345; }
