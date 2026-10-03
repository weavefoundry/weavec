// RFC 0032 §2.6: with -fno-weavec-runtime nothing is guarded: the facet stays unresolved and the access is not enforced.
// STAGE: S3
// The same program as heap-index-field_bug.c, at an index that stays inside the mapping.
// FLAGS: -fno-weavec-runtime
// RUN-INPUT: 9
// EXPECT-LEDGER: /config/runtime == false
// EXPECT-LEDGER: /summary/guarded == 0
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
  v->data[i] = x; // MISS: not enforced without the runtime // UNRESOLVED: spatial:inexpressible
}
int main(int argc, char **argv) {
  struct vec *v = vec_new(8);
  if (!v || !v->data || argc < 2) return 1;
  vec_set(v, (size_t)atoi(argv[1]), 42);
  return 0;
}
