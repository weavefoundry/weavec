// RFC 0042 amendment 15: an element of a flexible array member lies past its
// struct's size, so the struct's bounds do not cover it: the access keeps its
// guard, and a valid program runs.
// CLEAN
// FLAGS: -O2 -fweavec-safe
#include <weavec.h>
#include <stdlib.h>

struct node { int count; struct node *kids[]; };

int degree(const struct node *n, int i) {
  return n->kids[i] != NULL ? n->count : 0;
}

WEAVEC_GUARDED int main(void) {
  struct node *n = calloc(1, sizeof *n + 2 * sizeof(struct node *));
  if (n == NULL)
    return 1;
  n->count = 2;
  n->kids[1] = n;
  return degree(n, 1) == 2 ? 0 : 1;
}
