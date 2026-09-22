// RFC 0030 §7.2 (T p[n], a VLA parameter): the parameter is Counted(n) and nullable.
// STAGE: S6
// 'p' is declared with the earlier parameter 'n' as its bound, so it is Counted(n): each
// call is checked with the len template before the call, and p[i] for i < n is proven
// inside. The kind is nullable, so it does not discharge the null facet of p[i], which
// stays checked (the loop's inferred nonnull requirement is never trusted, §7.5). The run
// passes n == 6 for an int[4].
// RUN-INPUT: 6
// ASAN
#include <stddef.h>
#include <stdlib.h>

int sumv(size_t n, const int p[n]) {
  int s = 0;
  for (size_t i = 0; i < n; i++) s += p[i]; // NOT-PROVEN: null
  return s;
}

int main(int argc, char **argv) {
  int a[4] = {1, 2, 3, 4};
  size_t n = argc > 1 ? (size_t)atoi(argv[1]) : 4;
  return sumv(n, a); // BUG: out-of-bounds // TRAP: len
}
