// The twin of escaping-oob_bug.c, which stops at the last element. A
// pointer before its object, stored and passed, is no error.
// CLEAN
// RUN-INPUT:
// RUN-INPUT: 8
#include <stdio.h>
#include <stdlib.h>

struct vec {
  double *v; // 1-based: v[1] .. v[n]
  long n;
};

__attribute__((noinline)) static void make(struct vec *x, long n) {
  x->v = (double *)malloc(sizeof(double) * (size_t)n) - 1;
  x->n = n;
}

__attribute__((noinline)) static double sum(const struct vec *x, long upto) {
  double s = 0;
  for (long i = 1; i <= upto; i++)
    s += x->v[i];
  return s;
}

int main(int argc, char **argv) {
  struct vec a, b;
  make(&a, 8);
  make(&b, 8);
  for (long i = 1; i <= 8; i++)
    a.v[i] = b.v[i] = 1;
  long upto = argc > 1 ? atol(argv[1]) : 8;
  printf("%g\n", sum(&a, upto));
  free(a.v + 1);
  free(b.v + 1);
  return 0;
}
