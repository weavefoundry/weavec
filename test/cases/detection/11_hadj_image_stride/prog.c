// RFC 0034 detection set, case 11 (heap stride into a neighbour): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 64 4 10 6
#include <stdio.h>
#include <stdlib.h>

struct image {
  int w, h;
  unsigned char *px;
};

static struct image *image_new(int w, int h) {
  struct image *im = malloc(sizeof *im);
  if (!im)
    return NULL;
  im->w = w;
  im->h = h;
  im->px = calloc((size_t)w * (size_t)h, 1);
  if (!im->px) {
    free(im);
    return NULL;
  }
  return im;
}

static void image_set(struct image *im, int x, int y, unsigned char v) {
#ifdef FIX
  if (x < 0 || y < 0 || x >= im->w || y >= im->h)
    return;
#endif
  im->px[(size_t)y * (size_t)im->w + (size_t)x] = v; // STOP // MISS: the stride lands in the neighbouring live block, and the guard checks the accessed address
}

static unsigned image_sum(const struct image *im) {
  unsigned s = 0;
  for (int i = 0; i < im->w * im->h; i++)
    s += im->px[i];
  return s;
}

int main(int argc, char **argv) {
  if (argc < 5)
    return 2;
  int w = atoi(argv[1]), h = atoi(argv[2]);
  struct image *a = image_new(w, h);
  struct image *b = image_new(w, h);
  if (!a || !b)
    return 1;
  image_set(a, atoi(argv[3]), atoi(argv[4]), 200);
  printf("a=%u b=%u\n", image_sum(a), image_sum(b));
  free(a->px);
  free(a);
  free(b->px);
  free(b);
  return 0;
}
