// RFC 0041 §6 (S3), RFC 0043 (O5): a use after free the rules cannot see in
// a safe function (a reference loaded before a call released its block)
// reads the released block's own bytes, never another object's, and is not
// reported.
// CLEAN
// FLAGS: -O1
#include <weavec.h>
#include <stdlib.h>

struct box { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(n) data; size_t n; };
struct view { int *WEAVEC_COUNTED_BY(n) seen; size_t n; };

__attribute__((noinline)) WEAVEC_SAFE static void drop(struct box *b) {
  free(b->data);
  b->data = NULL;
  b->n = 0;
}

__attribute__((noinline)) WEAVEC_SAFE static int stale(struct box *b,
                                                       struct view *v) {
  int *seen = v->seen;
  drop(b);
  int *other = calloc(4, sizeof *other); // never the released block
  if (other == NULL)
    return 1;
  other[0] = 7;
  int r = seen[0] == 7;
  free(other);
  return r;
}

int main(void) {
  struct box b = {calloc(4, sizeof(int)), 4};
  if (b.data == NULL)
    return 1;
  struct view v = {b.data, 4};
  return stale(&b, &v);
}
