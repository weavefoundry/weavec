#include <stdlib.h>

struct node {
  int value;
};

static void node_free(struct node *n) {
  free(n);
}

int read_node(struct node *n) {
  int value = n->value;
  node_free(n);
  return value;
}
