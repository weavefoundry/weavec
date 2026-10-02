// RFC 0032 §8, Motivation: a callee stores its parameter into an element of a global array; the caller frees the object and another function reads the element.
// Before the fix every site was proven: the boundary did not name an element cell's place
// class, so the dangling pointer in 'g[1]' was no boundary fact and the read in 'fire'
// kept its entry assumption. Now the call of 'fire' and the read are not proven, and the
// guard traps.
// ASAN
#include <stdio.h>
#include <stdlib.h>
static int *g[4];
static int seen;
static void add(int *p) { g[1] = p; }
static void fire(void) {
  if (g[1]) seen += *g[1]; // BUG: use-after-free // NOT-PROVEN: temporal // TRAP: live
}
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(x);
  free(x);
  fire(); // NOT-PROVEN: temporal
  return seen == 0;
}
