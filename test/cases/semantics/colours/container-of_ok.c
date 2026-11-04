// RFC 0037 §8: arithmetic back from a member to its struct stays in one
// object, whatever the member's offset.
// RUN-INPUT:
// CLEAN
#include <stddef.h>
#include <stdlib.h>

struct node { long key; char pad[100]; struct node *next; long link; };

__attribute__((noinline)) static struct node *owner(long *link) {
  return (struct node *)((char *)link - offsetof(struct node, link));
}

int main(void) {
  struct node *n = calloc(1, sizeof *n);
  n->key = 3;
  return (int)owner(&n->link)->key - 3;
}
