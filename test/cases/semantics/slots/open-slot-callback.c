// RFC 0030 §9.3 and §5.1: an indirect call through an open slot with no known target is a callback default.
// STAGE: S7
// 'hook' has external linkage and nothing in this unit stores into it, so the slot is open
// and empty: the call gets the unknown-callee default with reason callback. The Call site
// and the later use of 'p' are unresolved(callback), with no diagnostic, and the callee
// operand's null facet is checked with the function-pointer form of nonnull.
#include <stdlib.h>

void (*hook)(char *);

int f(void) {
  char *p = malloc(8);
  if (!p) return 0;
  p[0] = 1;
  hook(p); // UNRESOLVED: temporal:callback // TRAP: nonnull
  return p[0]; // UNRESOLVED: temporal:callback
}
