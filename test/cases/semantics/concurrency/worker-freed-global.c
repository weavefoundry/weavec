// RFC 0030 §5.3: a worker using a global that main freed is not proven.
// STAGE: S4
// 'worker' is a pthread_create target that reads the global 'g', so 'g' is in G and the
// worker's own accesses through it are not proven (the Departure of §5.3 names this case):
// their temporal facet is trusted(concurrency), or a boundary reason once §9.4 propagates.
// main frees 'g' before starting the thread, so the read is a use-after-free, which ASan
// reports.
// ASAN
#include <pthread.h>
#include <stdlib.h>

static char *g;
static volatile char sink;

static void *worker(void *arg) {
  (void)arg;
  sink = g[0]; // BUG: use-after-free // NOT-PROVEN: temporal
  return NULL;
}

int main(void) {
  pthread_t t;
  g = malloc(8);
  if (!g) return 1;
  g[0] = 1;
  free(g);
  if (pthread_create(&t, NULL, worker, NULL) != 0) return 1;
  pthread_join(t, NULL);
  return 0;
}
