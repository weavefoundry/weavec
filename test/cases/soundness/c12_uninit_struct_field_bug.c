#include <stdlib.h>
struct cfg { int n; int *vals; };
static int sum(const struct cfg *c) { int s = 0; for (int i = 0; i < c->n; i++) s += c->vals[i]; return s; } // BUG: use-of-uninitialized // TRAP: nonnull
int main(void) {
  struct cfg c;
  c.n = 3;
  return sum(&c);
}
