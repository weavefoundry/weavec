// RFC 0032 §8: as alias-global-element-const_bug.c, with the element chosen by a counter the callee keeps: a weak store into the array's elements.
// ASAN
#include <stdio.h>
#include <stdlib.h>
static int *g[4];
static int n;
static int seen;
static void add(int *p) {
  if (n < 4) g[n++] = p;
}
static void fire(void) {
  for (int i = 0; i < n; i++)
    seen += *g[i]; // BUG: use-after-free // TRAP
}
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(x);
  free(x);
  fire();
  return seen == 0;
}
