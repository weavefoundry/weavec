// Element pointer kept across a push that may reallocate.
// ASAN
#include <stdlib.h>
struct vec { int *data; size_t len, cap; };
static int vec_push(struct vec *v, int x) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 4;
    int *nd = realloc(v->data, nc * sizeof *nd);
    if (!nd) return -1;
    v->data = nd; v->cap = nc;
  }
  v->data[v->len++] = x;
  return 0;
}
int main(void) {
  struct vec v = {0};
  if (vec_push(&v, 1)) return 1;
  int *first = &v.data[0];
  for (int i = 0; i < 100; i++) if (vec_push(&v, i)) break;
  int r = *first; // BUG: use-after-move
  free(v.data);
  return r;
}
