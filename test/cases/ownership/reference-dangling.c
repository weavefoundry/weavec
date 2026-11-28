// RFC 0043 §7: a reference (a pointer field that is not WEAVEC_OWNED) to a
// block its owner released stops the program where a safe function loads
// and uses it, in every check mode of the checked build. RFC 0055 §2 (D2):
// the fast build believes it (test/Guards/reference-load.c).
// FLAGS: -O2 -fweavec-safe -fweavec-safe-checks=full
#include <weavec.h>
#include <stdlib.h>

struct item { int v; };
struct cache { struct item *last; };

__attribute__((noinline)) void remember(struct cache *c) {
  struct item *it = calloc(1, sizeof *it);
  if (it == NULL)
    return;
  it->v = 6;
  c->last = it;
  free(it);
}

__attribute__((noinline)) int recall(const struct cache *c) {
  return c->last->v; // TRAP: heap-use-after-free
}

WEAVEC_GUARDED int main(void) {
  struct cache c = {0};
  remember(&c);
  return recall(&c);
}
