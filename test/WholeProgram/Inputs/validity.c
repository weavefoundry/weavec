// The other unit of rfc0008-validity.c.
#include "validity.h"

#include <stdlib.h>
#include <string.h>

int vec_grow(struct vec *v) {
  int *bigger = realloc(v->items, sizeof *bigger * (v->cap + 8));
  if (!bigger)
    return 0;
  v->items = bigger;
  v->cap += 8;
  return 1;
}

void vec_reset(struct vec *v) {
  free(v->items);
  v->items = NULL;
}

char *find(const char *s, char c) { return strchr(s, c); }

int node_open(struct node **out) {
  *out = malloc(sizeof **out);
  return *out != NULL;
}

int node_value(const struct node *n) { return n->value; }
