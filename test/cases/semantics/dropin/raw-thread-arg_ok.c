// RFC 0033 §2: a number passed as a thread argument through a wrapper (libuv's
// uv_thread_create, its tests' `(void *)42`) is no raw-pointer error.
// STAGE: S1
// CLEAN
// RUN-INPUT:
// FLAGS: -pthread
#include <pthread.h>
#include <stdint.h>
typedef void (*entry_fn)(void *);
struct start { entry_fn fn; void *arg; };
static void *trampoline(void *p) {
  struct start *s = p;
  s->fn(s->arg);
  return NULL;
}
static int thread_create(pthread_t *t, struct start *s) {
  return pthread_create(t, NULL, trampoline, s);
}
static int seen;
static void worker(void *arg) { seen = (int)(uintptr_t)arg; }
int main(void) {
  pthread_t t;
  struct start s = {worker, (void *)42};
  if (thread_create(&t, &s) != 0) return 1;
  pthread_join(t, NULL);
  return seen == 42 ? 0 : 1;
}
