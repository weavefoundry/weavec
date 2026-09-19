// RFC 0030 §6.3: the dangling-escape reason is an error only under a require level.
// STAGE: S7
// The boundary rows of boundary/remember-free-peek.c are reasons, not diagnostics. Under
// -fweavec-require=checked each such unresolved facet is an unresolved-operation error: at
// the call to 'peek', where 'g_cache' dangles, and at the propagated use inside 'peek'.
// FLAGS: -fweavec-require=checked
#include <stdlib.h>

static char *g_cache;

static void remember(char *p) { g_cache = p; }

static int peek(void) { return g_cache[0]; } // BUG: unresolved-operation definite

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 3;
  remember(p);
  free(p);
  return peek(); // BUG: unresolved-operation definite
}
