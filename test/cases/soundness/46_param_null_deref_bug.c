// Null passed from a caller whose value comes from a loaded field (no fact).
// ASAN
#include <stdlib.h>
struct cfg { char *name; };
static int first(const char *s) { return s[0]; }
int main(void) {
  struct cfg *c = calloc(1, sizeof *c);
  if (!c) return 1;
  int r = first(c->name); // BUG: null-dereference
  free(c);
  return r;
}
