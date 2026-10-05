// RFC 0034 detection set, case 41 (null dereference): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 2000000000 2000000000
#include <stdio.h>
#include <stdlib.h>

struct canvas {
  size_t w, h;
  unsigned char *px;
};

static int canvas_init(struct canvas *c, size_t w, size_t h) {
  c->w = w;
  c->h = h;
  c->px = malloc(w * h);
#ifdef FIX
  if (!c->px)
    return -1;
#endif
  return 0;
}

static void canvas_plot(struct canvas *c, size_t x, size_t y) {
  c->px[y * c->w + x] = 255; // STOP
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  struct canvas c;
  if (canvas_init(&c, strtoull(argv[1], NULL, 10), strtoull(argv[2], NULL, 10))) {
    printf("canvas too large\n");
    return 0;
  }
  canvas_plot(&c, 1, 0);
  printf("plotted\n");
  free(c.px);
  return 0;
}
