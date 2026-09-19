// RFC 0030 §7.3: a call passing a cursor to a parameter that relies on Single gets a Call-site row.
// STAGE: S6
// 'peek' is exported, so 'p' is Single-or-nullable by A1 and p[0] is proven only by that
// default: 'p' is reliesOnSingle. The call passes 'a + k', which is neither Single-valid nor
// proven to have an element, so its Call site gets spatial unresolved(unknown-extent), never
// a check (a correct caller may pass a one-past-the-end value the callee does not read). The
// run passes k == 4; ASan reports the read in 'peek', whose enclosing call is the row.
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>

int peek(const int *p) { return p[0]; }

int main(int argc, char **argv) {
  int a[4] = {1, 2, 3, 4};
  int k = argc > 1 ? atoi(argv[1]) : 0;
  return peek(a + k); // BUG: out-of-bounds // UNRESOLVED: spatial:unknown-extent
}
