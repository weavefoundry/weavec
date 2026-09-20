// RFC 0030 §5.3: a signal handler racing a null test; the use after the test is checked.
// STAGE: S4
// 'handler' is a signal entry target that frees 'g_buf' and nulls it, so 'g_buf' is in G.
// A null test followed by a use does not prove the use, because the handler can run in
// between: the use's null facet is checked (the check tests the value the access uses) and
// its temporal facet is trusted(concurrency) (the Departure of §5.3). The raise() between
// the test and the use makes the race deterministic, and the check traps.
// RUN-INPUT:
// ASAN
#include <signal.h>
#include <stdlib.h>

static char *g_buf;

static void handler(int sig) {
  (void)sig;
  free(g_buf);
  g_buf = NULL;
}

int main(void) {
  g_buf = malloc(8);
  if (!g_buf) return 1;
  signal(SIGUSR1, handler);
  if (g_buf) {
    raise(SIGUSR1);
    g_buf[0] = 1; // BUG: null-dereference // TRAP: nonnull // TRUSTED: temporal:concurrency
  }
  return 0;
}
