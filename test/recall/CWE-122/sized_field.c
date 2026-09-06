// CWE-122: heap-based buffer overflow through a struct's pointer field, whose
// size is a sibling field (RFC 0012, *Sized fields*): once annotated, and
// once inferred from the function that fills the pair.
#include "recall.h"

struct buf {
  char *__attribute__((annotate("weavec.sized_by.cap"))) data;
  size_t cap;
};

void bad(struct buf *b) {
  b->data[b->cap] = 0; // RECALL: out-of-bounds
}

struct vec {
  int *items;
  size_t n;
  size_t cap;
};

void vec_init(struct vec *v, size_t cap) {
  v->items = malloc(cap * sizeof *v->items);
  v->cap = cap;
  v->n = 0;
}

int bad_inferred(struct vec *v) {
  return v->items[v->cap]; // RECALL: out-of-bounds
}

void good(struct buf *b, struct vec *v) {
  if (b->cap > 0)
    b->data[b->cap - 1] = 0;
  for (size_t i = 0; i < b->cap; i++)
    b->data[i] = 0;
  if (v->n < v->cap)
    v->items[v->n] = 1;
}
