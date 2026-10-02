// RFC 0032 §3, §6: the correct twin of heap-index-field_bug.c: an index inside the block passes its guard.
// STAGE: S3
// CLEAN
// RUN-INPUT: 7
// ASAN
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
  v->data[i] = x; // GUARDED: spatial
}
int main(int argc, char **argv) {
  struct vec *v = vec_new(8);
  if (!v || !v->data || argc < 2) return 1;
  vec_set(v, (size_t)atoi(argv[1]), 42);
  free(v->data);
  free(v);
  return 0;
}
