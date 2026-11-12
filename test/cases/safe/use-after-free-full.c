// RFC 0041 §7, RFC 0043 (O5): -fweavec-safe-checks=full keeps a safe
// function's guards, so a use after free the rules cannot see (a reference
// loaded before a call released its block) is reported as in guarded code.
// FLAGS: -O1 -fweavec-safe-checks=full
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
  return seen[0]; // TRAP: heap-use-after-free
}

int main(void) {
  struct box b = {calloc(4, sizeof(int)), 4};
  if (b.data == NULL)
    return 1;
  struct view v = {b.data, 4};
  return stale(&b, &v);
}
