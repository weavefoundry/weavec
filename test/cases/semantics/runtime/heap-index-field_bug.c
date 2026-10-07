// RFC 0032 §3, §6: a subscript whose extent the analysis does not know is guarded against the object its base points into.
// STAGE: S3
// 'v->data' is a heap block of 8 ints; nothing ties its extent to 'v->cap' statically, so
// the spatial facet of 'v->data[i]' is not proven and no static check can name a bound. The
// guard looks the block up from the pointer and traps on an index outside it, however far.
// RUN-INPUT: 100000
#include <stdio.h>
#include <stdlib.h>
struct vec { int *data; size_t len, cap; };
static struct vec *vec_new(size_t cap) {
  struct vec *v = malloc(sizeof *v);
  if (!v) return NULL;
  v->data = malloc(cap * sizeof(int));
  v->len = 0;
  v->cap = cap;
  return v;
}
static void vec_set(struct vec *v, size_t i, int x) {
  v->data[i] = x; // BUG: out-of-bounds // TRAP
}
int main(int argc, char **argv) {
  struct vec *v = vec_new(8);
  if (!v || !v->data || argc < 2) return 1;
  vec_set(v, (size_t)atoi(argv[1]), 42);
  free(v->data);
  free(v);
  return 0;
}
