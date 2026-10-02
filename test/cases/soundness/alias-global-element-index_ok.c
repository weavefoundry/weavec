// RFC 0032 §8: the correct twin of alias-global-element-index_bug.c: the elements are read while their objects live.
// CLEAN
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
    seen += *g[i];
}
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(x);
  fire();
  n = 0;
  g[0] = NULL;
  free(x);
  return seen != 7;
}
