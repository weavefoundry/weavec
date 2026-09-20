// Data race / UAF across threads.
// ASAN
#include <pthread.h>
#include <stdlib.h>
static char *shared;
static void *worker(void *arg) { (void)arg; free(shared); return NULL; }
int main(void) {
  shared = malloc(8);
  if (!shared) return 1;
  shared[0] = 1;
  pthread_t t;
  if (pthread_create(&t, NULL, worker, NULL) != 0) return 1;
  pthread_join(t, NULL);
  return shared[0]; // BUG: use-after-free // NOT-PROVEN: temporal
}
