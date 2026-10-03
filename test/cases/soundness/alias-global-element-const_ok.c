// RFC 0032 §8: the correct twin of alias-global-element-const_bug.c: the element is read before the object is freed, and cleared.
// CLEAN
// ASAN
#include <stdio.h>
#include <stdlib.h>
static int *g[4];
static int seen;
static void add(int *p) { g[1] = p; }
static void fire(void) {
  if (g[1]) seen += *g[1];
}
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(x);
  fire();
  g[1] = NULL;
  free(x);
  return seen != 7;
}
