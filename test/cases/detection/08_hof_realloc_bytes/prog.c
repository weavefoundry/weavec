// RFC 0034 detection set, case 08 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 5 6 7 8 9 10
#include <stdio.h>
#include <stdlib.h>

struct intvec {
  int *data;
  size_t len, cap;
};

static int vec_push(struct intvec *v, int x) {
  if (v->len == v->cap) {
    size_t ncap = v->cap ? v->cap * 2 : 4;
#ifdef FIX
    int *nd = realloc(v->data, ncap * sizeof *nd);
#else
    int *nd = realloc(v->data, ncap); /* bytes, not elements */
#endif
    if (!nd)
      return -1;
    v->data = nd;
    v->cap = ncap;
  }
  v->data[v->len++] = x; // STOP
  return 0;
}

int main(int argc, char **argv) {
  struct intvec v = {0};
  for (int i = 1; i < argc; i++)
    if (vec_push(&v, atoi(argv[i])) != 0)
      return 1;
  long sum = 0;
  for (size_t i = 0; i < v.len; i++)
    sum += v.data[i];
  printf("len=%zu sum=%ld\n", v.len, sum);
  free(v.data);
  return 0;
}
