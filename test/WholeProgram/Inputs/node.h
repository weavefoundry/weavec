/* A tiny library split across units, for the RFC 0005 tests. */
#ifndef WEAVEC_TEST_NODE_H
#define WEAVEC_TEST_NODE_H

struct node {
  int v;
  char *name;
};

struct node *node_new(void);
void node_free(struct node *n);
int *node_vp(struct node *n);
void node_set_name(struct node *n, char *name);

#endif /* WEAVEC_TEST_NODE_H */
