#include <stdlib.h>

struct node {
  int value;
};

static void node_free(struct node *n) {
  free(n);
}

int read_node(struct node *n) {
  struct node *alias = n;
  node_free(alias);
  return n->value;
}
