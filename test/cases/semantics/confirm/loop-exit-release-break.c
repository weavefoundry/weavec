// RFC 0034, Silent misses (a release in a loop left early), a variant of
// loop-exit-release.c: the count comes before the array in the record and
// the loop is left by `break`. The element read `p->kids[j]` shares its
// position (offset modulo the element size) with `n`, and its index is
// bounded only by `n`; the read must stay a pointer so that closer()'s
// summary keeps the release. The default build must stop at the read;
// -DFIX reads an element closer() does not free.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 0
#include <stdio.h>
#include <stdlib.h>
struct w { int x; };
struct panel { int n; struct w *kids[4]; };
static void add(struct panel *p, int x) {
  struct w *w = calloc(1, sizeof *w);
  if (!w) exit(1);
  w->x = x;
  p->kids[p->n++] = w;
}
static void closer(struct panel *p, int x) {
  for (int j = 0; j < p->n; j++)
    if (j == x) { free(p->kids[j]); break; }
}
int main(int argc, char **argv) {
  struct panel p = {0, {0}};
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
