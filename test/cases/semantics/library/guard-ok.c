// RFC 0030 §9.2: a guard that returns 1 without testing its argument proves nothing about it.
// STAGE: S4
// On the k != 0 path 'ok' returns 1 without testing 'p', so the class set K of paths where
// 'p' is not proven non-null contains both result classes and no non-null fact is derived.
// After 'if (!ok(p, 1)) return 0;', p->v keeps a checked null facet. The run passes null.
// RUN-INPUT:
#include <stddef.h>

struct x { int v; };

int ok(struct x *p, int k) { if (k) return 1; if (!p) return 0; return 1; }

int use(struct x *p) {
  if (!ok(p, 1)) return 0;
  return p->v; // TRAP: nonnull
}

int main(int argc, char **argv) {
  struct x v = {7};
  (void)argv;
  return use(argc > 1 ? &v : NULL);
}
