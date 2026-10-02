// RFC 0031 §4.1, §4.7 I1, I4: a store through a pointer that may point to either of two cells is a weak update: each cell keeps its old value and may hold the new one.
// STAGE: S2
// 'p' points to 'a' or to 'b' depending on the input, and NULL is stored through it. Both
// 'a' and 'b' may then be null, so neither dereference may be proven non-null: the run with
// an argument makes 'a' null and must trap at its dereference.
// RUN-INPUT: 1
// ASAN
#include <stdio.h>
int main(int argc, char **argv) {
  (void)argv;
  int x = 1, y = 2;
  int *a = &x, *b = &y;
  int **p = argc > 1 ? &a : &b;
  *p = NULL;
  int r = *a; // BUG: null-dereference // TRAP: nonnull
  printf("%d\n", r);
  return 0;
}
