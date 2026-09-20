// CWE-122: heap-based buffer overflow through a struct's pointer field, whose
// size is a sibling field (RFC 0012, *Sized fields*): once annotated, and
// once inferred from the function that fills the pair.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

struct buf {
  char *__attribute__((annotate("weavec.sized_by.cap"))) data;
  size_t cap;
};

void bad(struct buf *b) {
  b->data[b->cap] = 0; // BUG: out-of-bounds // TRAP: index
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
  return v->items[v->cap]; // BUG: out-of-bounds // TRAP: index
}

void good(struct buf *b, struct vec *v) {
  if (b->cap > 0)
    b->data[b->cap - 1] = 0;
  for (size_t i = 0; i < b->cap; i++)
    b->data[i] = 0;
  if (v->n < v->cap)
    v->items[v->n] = 1;
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
static struct buf b;
static struct vec v;
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  b.cap = 4;
  b.data = malloc(b.cap);
  if (!b.data)
    return 1;
  vec_init(&v, 4);
  if (!v.items) {
    free(b.data);
    return 1;
  }
  good(&b, &v);
  if (which == 1) bad(&b);
  if (which == 2) bad_inferred(&v);
  free(v.items);
  free(b.data);
  return 0;
}
