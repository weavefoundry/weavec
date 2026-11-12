// RFC 0043 §7: a reference only compared (or copied) is not tested, so a
// dangling one that is never followed does not stop the program.
// CLEAN
// FLAGS: -O2 -fweavec-safe
#include <weavec.h>
#include <stdlib.h>

struct item { int v; };
struct cache { struct item *last; };

__attribute__((noinline)) int remembered(const struct cache *c,
                                         const struct item *it) {
  return c->last == it;
}

WEAVEC_GUARDED int main(void) {
  struct item *it = calloc(1, sizeof *it);
  struct cache c = {it};
  free(it);
  it = calloc(1, sizeof *it);
  int r = remembered(&c, it);
  free(it);
  return r > 1;
}
