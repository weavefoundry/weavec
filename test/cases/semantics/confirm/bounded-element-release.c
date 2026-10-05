// RFC 0034, Silent misses (a release in a loop left early), the
// straight-line form: closer() frees the element its argument selects
// after testing it against the count field, so the index is bounded by
// that field only. Its summary must keep the release; the default build
// must stop at the caller's read. -DFIX reads an element closer() does
// not free.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 0
#include <stdio.h>
#include <stdlib.h>
struct w { int x; };
struct panel { struct w *kids[4]; int n; };
static void closer(struct panel *p, int j) {
  if (j >= 0 && j < p->n)
    free(p->kids[j]);
}
int main(int argc, char **argv) {
  struct panel p = {{0}, 0};
  for (int i = 0; i < 4; i++) {
    p.kids[i] = calloc(1, sizeof(struct w));
    if (!p.kids[i]) return 1;
    p.n++;
  }
#ifdef FIX
  struct w *f = p.kids[3];
#else
  struct w *f = p.kids[0];
#endif
  closer(&p, atoi(argv[1]));
  printf("%d\n", f->x); // STOP
  return argc > 5;
}
