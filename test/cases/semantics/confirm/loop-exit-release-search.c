// RFC 0034, Silent misses (a release in a loop left early), reduced from
// detection case 24 and probe 62: closer() searches the elements by a
// field of what each points to, frees the match and returns. The loop's
// test reads the count `n`, which the loop head folds into the element
// ranges beside `kids`, so every later read of `p->kids[i]` may be that
// integer; the read must stay a pointer for the summary to keep the
// release. The default build must stop at the caller's read; -DFIX reads
// an element closer() does not free.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 0
#include <stdio.h>
#include <stdlib.h>
struct w { int id; };
struct panel { struct w *kids[8]; int n; };
static void closer(struct panel *p, int id) {
  for (int i = 0; i < p->n; i++)
    if (p->kids[i]->id == id) { free(p->kids[i]); return; }
}
int main(int argc, char **argv) {
  struct panel p = {{0}, 0};
  for (int i = 0; i < 4; i++) {
    p.kids[i] = calloc(1, sizeof(struct w));
    if (!p.kids[i]) return 1;
    p.kids[i]->id = i;
    p.n++;
  }
#ifdef FIX
  struct w *f = p.kids[3];
#else
  struct w *f = p.kids[0];
#endif
  closer(&p, atoi(argv[1]));
  printf("%d\n", f->id); // STOP
  return argc > 5;
}
