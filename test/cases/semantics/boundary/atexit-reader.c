// RFC 0030 §9.4 and §5.3: 'free(g); exit(0)' with an atexit reader leaves 'g' dangling at the exit call.
// STAGE: S7
// An at-exit target is analysed as an entry point but does not contribute to G. The call to
// 'exit' is a boundary (a call that does not return), and 'g' holds a released pointer
// there: unresolved(dangling-escape). Propagation makes the reader's g[0], the
// ASan-reported bug site, unresolved(dangling-escape) instead of proven.
// ASAN
#include <stdlib.h>

static char *g;
static volatile char sink;

static void reader(void) { sink = g[0]; } // BUG: use-after-free // UNRESOLVED: temporal:dangling-escape

int main(void) {
  g = malloc(8);
  if (!g) return 1;
  g[0] = 'x';
  if (atexit(reader) != 0) return 1;
  free(g);
  exit(0); // UNRESOLVED: temporal:dangling-escape
}
