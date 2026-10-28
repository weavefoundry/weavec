// A 1-based array (a pointer before its object, stored and passed) read one
// past its end in another function: the byte after the object is its
// slot's tail, so the guard stops it. (Bounds of a pointer stored outside
// its object were the first draft of RFC 0036; the shadow suffices here.)
// RUN-INPUT:
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
    s += x->v[i]; // TRAP: heap-buffer-overflow
  return s;
}

int main(int argc, char **argv) {
  struct vec a, b;
  make(&a, 8);
  make(&b, 8);
  for (long i = 1; i <= 8; i++)
    a.v[i] = b.v[i] = 1;
  long upto = argc > 1 ? atol(argv[1]) : 9;
  printf("%g\n", sum(&a, upto));
  return 0;
}
