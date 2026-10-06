// RFC 0034 detection set, case 25 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 7 8 9 10 11 12
#include <stdio.h>
#include <stdlib.h>

struct vec {
  int *data;
  size_t len, cap;
};

static int vec_push(struct vec *v, int x) {
  if (v->len == v->cap) {
    size_t ncap = v->cap ? v->cap * 2 : 2;
    int *nd = realloc(v->data, ncap * sizeof *nd);
    if (!nd)
      return -1;
    v->data = nd;
    v->cap = ncap;
  }
  v->data[v->len++] = x;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct vec v = {0};
  if (vec_push(&v, atoi(argv[1])))
    return 1;
#ifdef FIX
  size_t first = 0;
#else
  int *first = &v.data[0]; /* pointer into the buffer */
#endif
  for (int i = 2; i < argc; i++)
    if (vec_push(&v, atoi(argv[i])))
      return 1;
#ifdef FIX
  v.data[first] += 1000;
  printf("first=%d len=%zu\n", v.data[first], v.len);
#else
  *first += 1000; // STOP
  printf("first=%d len=%zu\n", *first, v.len);
#endif
  free(v.data);
  return 0;
}
