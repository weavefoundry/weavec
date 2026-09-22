// RFC 0030 §4 "Trusted, concurrency": probe 15, whose worker frees a global main still uses.
// STAGE: S4
// 'worker' is a pthread_create target that frees the global 'shared', so 'shared' is in G
// (§5.3). main's accesses through 'shared' have temporal trusted(concurrency), and so do
// the accesses inside 'worker'; the summary line counts them under A4. The null test before
// the last use does not prove it: that null facet is checked. The use-after-free stays a
// ledger row, which ASan confirms.
// ASAN
#include <pthread.h>
#include <stdlib.h>

static char *shared;

static void *worker(void *arg) {
  (void)arg;
  shared[0] = 2; // TRUSTED: temporal:concurrency
  free(shared);
  return NULL;
}

int main(void) {
  pthread_t t;
  shared = malloc(8);
  if (!shared) return 1;
  shared[0] = 1; // TRUSTED: temporal:concurrency
  if (pthread_create(&t, NULL, worker, NULL) != 0) return 1;
  pthread_join(t, NULL);
  if (shared) return shared[0]; // BUG: use-after-free // TRUSTED: temporal:concurrency // NOT-PROVEN: null
  return 0;
}
