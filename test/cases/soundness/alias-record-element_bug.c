// RFC 0032 §8: the same through an array member of a record: the element cell 'r->slot[1]' is of the class of its field.
// ASAN
#include <stdio.h>
#include <stdlib.h>
struct reg { int count; int *slot[4]; };
static struct reg registry;
static int seen;
static void add(struct reg *r, int *p) { r->slot[1] = p; }
static void fire(const struct reg *r) {
  if (r->slot[1]) seen += *r->slot[1]; // BUG: use-after-free // TRAP
}
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(&registry, x);
  free(x);
  fire(&registry);
  return seen == 0;
}
