/* The stores that fix `struct vec`'s pair (RFC 0012, *Sized fields*,
   "Inference"), and the ones that refute `struct view`'s. */
#include "../../Inputs/prelude.h"
#include "vec.h"

int vec_init(struct vec *v, size_t cap) {
  v->items = malloc(cap * sizeof *v->items);
  if (!v->items)
    return -1;
  v->cap = cap;
  v->n = 0;
  return 0;
}

int vec_push(struct vec *v, int x) {
  if (v->n == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 4;
    int *items = realloc(v->items, cap * sizeof *items);
    if (!items)
      return -1;
    v->items = items;
    v->cap = cap;
  }
  v->items[v->n++] = x;
  return 0;
}

void vec_free(struct vec *v) {
  free(v->items);
  v->items = NULL;
  v->cap = 0;
  v->n = 0;
}

void view_own(struct view *w, size_t len) {
  w->raw = malloc(len * sizeof *w->raw);
  w->len = len;
}

void view_borrow(struct view *w, int *p, size_t len) {
  w->raw = p;
  w->len = len;
}
