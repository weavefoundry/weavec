// RFC 0034, Silent misses: the false proof of the milestone's blind detection
// set (build/eval-2026-10-04/detect/findings/false_proof_loop_exit_release.c,
// found from detection cases 24 and 62). closer() frees an element inside a
// loop bounded by a field and leaves the loop early, and its summary lost the
// release, so main's alias f kept a proven temporal facet and the default
// build read freed memory (only verify mode trapped). The default build must
// stop at the read; -DFIX reads an element closer() does not free.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 0
#include <stdio.h>
#include <stdlib.h>
struct w { int x; };
struct panel { struct w *kids[4]; int n; };
static void add(struct panel *p, int x) {
  struct w *w = calloc(1, sizeof *w);
  if (!w) exit(1);
  w->x = x;
  p->kids[p->n++] = w;
}
static void closer(struct panel *p, int x) {
  for (int j = 0; j < p->n; j++)
    if (j == x) { free(p->kids[j]); return; } /* `break` loses it too */
}
int main(int argc, char **argv) {
  struct panel p = {{0}, 0};
  for (int i = 0; i < 4; i++) add(&p, i);
#ifdef FIX
  struct w *f = p.kids[3];
#else
  struct w *f = p.kids[0];
#endif
  closer(&p, atoi(argv[1]));
  printf("%d\n", f->x); // STOP
  return argc > 5;
}
