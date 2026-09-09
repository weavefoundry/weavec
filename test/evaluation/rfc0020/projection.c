// RFC 0020: distinct call routes to shared origins, plus transitive trust.
// Compared against the preserved pre-projection binary by validation tooling.
#include "weavec.h"
static int first(void) { int x; return x; }
static int second(void) { int y; return y; }
static int middle(int n) {
  if (n) return first() + second();
  return second() + first();
}
static int diamond(int n) { return middle(n) + middle(!n); }
static int trusted(int n) {
  WEAVEC_UNSAFE { return diamond(n); }
}
int main(void) { return diamond(1) + trusted(0); }
