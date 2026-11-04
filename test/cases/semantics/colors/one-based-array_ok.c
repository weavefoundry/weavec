// RFC 0037 §8.3: a pointer biased one element before its block (a one-based
// array) lies in the slot before it; its accesses are not jumps.
// RUN-INPUT:
// CLEAN
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  double *x[8];
  double sum = 0;
  for (int i = 0; i < 8; i++)
    x[i] = (double *)malloc(9 * sizeof(double)) - 1;
  for (int k = 0; k < 8; k++)
    for (long i = 1; i <= 9; i++)
      x[k][i] = (double)i;
  for (int k = 0; k < 8; k++)
    for (long i = 1; i <= 9; i++)
      sum += x[k][i];
  printf("%g\n", sum);
  return 0;
}
