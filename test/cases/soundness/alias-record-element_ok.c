// RFC 0032 §8: the correct twin of alias-record-element_bug.c.
// CLEAN
// ASAN
#include <stdio.h>
#include <stdlib.h>
struct reg { int count; int *slot[4]; };
static struct reg registry;
static int seen;
static void add(struct reg *r, int *p) { r->slot[1] = p; }
static void fire(const struct reg *r) {
  if (r->slot[1]) seen += *r->slot[1];
}
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(&registry, x);
  fire(&registry);
  registry.slot[1] = NULL;
  free(x);
  return seen != 7;
}
