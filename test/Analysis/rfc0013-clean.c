// RFC 0013: clean counterparts for nullable, shared and cyclic construction.
// RUN: %weavec %s -- 2>&1 | count 0
#include "../evaluation/Inputs/heap.c"

struct pair { char *a, *b; struct pair *self; };
struct pair *pair_new(void) {
  struct pair *p = malloc(sizeof *p); if (!p) return NULL;
  p->a = malloc(4); if (!p->a) { free(p); return NULL; }
  p->b = p->a; p->self = p; return p;
}
void good(void) {
  struct pair *a = pair_new(), *b = pair_new();
  if (a) { a->b[3] = 0; free(a->a); free(a); }
  if (b) { b->a[3] = 0; free(b->b); free(b); }
}
void clean_borrow(void) {
  char local[4]; struct box *b = box_wrap(local);
  if (!b) return;
  b->data[3] = 0; free(b);
}
void snapshots(size_t n, int count) {
  for (int k = 0; k < count; ++k) {
    size_t size = n; char *p = malloc(n);
    n = k + 1;
    if (p) for (size_t i = 0; i < size; ++i) p[i] = 0;
    free(p);
  }
}
