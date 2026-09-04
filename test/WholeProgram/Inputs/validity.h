// Interface of validity.c: no annotations, everything is inferred from the
// definitions in that unit (RFC 0005) and travels in the program database.
#ifndef VALIDITY_H
#define VALIDITY_H

struct vec {
  int *items;
  int cap;
};

struct node {
  int value;
};

/// Grows `v->items` in place; returns nonzero on success.
int vec_grow(struct vec *v);
/// Releases `v->items` and nulls it.
void vec_reset(struct vec *v);
/// A pointer into `s` at the first `c`, or null.
char *find(const char *s, char c);
/// Allocates a node into `*out`; returns nonzero on success.
int node_open(struct node **out);
/// Reads the node.
int node_value(const struct node *n);

#endif
