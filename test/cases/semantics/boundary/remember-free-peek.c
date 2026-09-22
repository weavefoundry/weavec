// RFC 0030 §9.4: 'remember(p); free(p); peek()' leaves a global dangling at the call to 'peek'.
// STAGE: S7
// At the call to 'peek' the global 'g_cache' may hold the released pointer, so that Call
// site is unresolved(dangling-escape). Propagation then downgrades every temporal facet that
// relied on the entry assumption for 'g_cache': g_cache[0] inside 'peek', the ASan-reported
// bug site, is unresolved(dangling-escape) instead of proven (probe 02).
// ASAN
#include <stdlib.h>

static char *g_cache;

static void remember(char *p) { g_cache = p; }

static int peek(void) { return g_cache[0]; } // BUG: use-after-free // UNRESOLVED: temporal:dangling-escape

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 3;
  remember(p);
  free(p);
  return peek(); // UNRESOLVED: temporal:dangling-escape
}
