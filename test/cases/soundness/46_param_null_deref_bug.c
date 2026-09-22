// Null passed from a caller whose value comes from a loaded field (no fact).
// ASAN
#include <stdlib.h>
struct cfg { char *name; };
static int first(const char *s) { return s[0]; }
int main(void) {
  struct cfg *c = calloc(1, sizeof *c);
  if (!c) return 1;
  // RFC 0030 §7.5: first() is static and must-accesses s[0], so each call checks its
  // argument for null (the engine does not know c->name is zero-filled).
  int r = first(c->name); // BUG: null-dereference // TRAP: nonnull
  free(c);
  return r;
}
