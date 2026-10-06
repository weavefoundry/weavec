// RFC 0034 detection set, case 51 (type confusion): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: rect 3 4
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum kind { CIRCLE, RECT };

struct shape {
  enum kind k;
};
struct circle {
  enum kind k;
  double r;
};
struct rect {
  enum kind k;
  double w, h, x, y;
};

static void *shape_new(enum kind k) {
#ifdef FIX
  size_t sz = k == CIRCLE ? sizeof(struct circle) : sizeof(struct rect);
#else
  size_t sz = sizeof(struct circle); /* every shape was a circle once */
#endif
  struct shape *s = calloc(1, sz);
  if (s)
    s->k = k;
  return s;
}

static double area(const struct shape *s) {
  if (s->k == CIRCLE) {
    const struct circle *c = (const struct circle *)s;
    return 3.14159 * c->r * c->r;
  }
  const struct rect *r = (const struct rect *)s;
  return r->w * r->h;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  enum kind k = strcmp(argv[1], "rect") == 0 ? RECT : CIRCLE;
  struct shape *s = shape_new(k);
  if (!s)
    return 1;
  if (k == RECT) {
    struct rect *r = (struct rect *)s;
    r->w = atof(argv[2]);
    r->h = argc > 3 ? atof(argv[3]) : 1.0; // STOP
    r->x = r->y = 0;
  } else {
    ((struct circle *)s)->r = atof(argv[2]);
  }
  printf("area %.2f\n", area(s));
  free(s);
  return 0;
}
