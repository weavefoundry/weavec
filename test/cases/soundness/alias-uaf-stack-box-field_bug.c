// RFC 0031 Motivation, probe p4 b2 (build/rfc31/probes/p4.c): a stack 'box' holds 'o' in its
// field 'a'; 'q' points to the box; 'o' is freed and read back as 'q->a->v'.
// v0.11.0 proves the temporal facet of 'q->a->v' (a false proof, ASan-confirmed; it checks
// only the null facet). The line also loads 'q->a' from the live box (legitimately proven),
// so the bug marker needs a diagnostic; see alias-uaf-stack-cell_bug.c.
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
struct box { struct n *a; };
int b2(void) {
  struct box bx;
  struct n *o = malloc(sizeof *o);
  if (!o) abort();
  bx.a = o;
  struct box *q = &bx;
  free(o);
  return q->a->v; // BUG: use-after-free
}
int main(void) { return b2() == 12345; }
