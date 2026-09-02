/* Definitions for node.h. Analysed as its own unit; its summaries are what
 * the other units of a test see (RFC 0005). */
#include "../../Inputs/prelude.h"
#include "node.h"

struct node *node_new(void) {
  struct node *n = malloc(sizeof(struct node));
  if (n)
    n->name = NULL;
  return n;
}

void node_free(struct node *n) {
  if (!n)
    return;
  free(n->name);
  free(n);
}

int *node_vp(struct node *n) { return &n->v; }

void node_set_name(struct node *n, char *name) {
  free(n->name);
  n->name = name;
}
