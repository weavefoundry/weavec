// RFC 0032 §3: a use-after-free through a table of callbacks traps in the callback.
// STAGE: S3
// The argument registered for 'show' is freed before the timers fire. One of the eight
// probes of RFC 0032's Motivation. The callback's own temporal facet is proven under the
// entry assumption of its parameter; the boundary that breaks it is the call of 'fire', and
// the guard of the unknown extent finds the dead object.
// RUN-INPUT:
// ASAN
#include <stdio.h>
#include <stdlib.h>
typedef void (*cb_t)(void *);
struct timer { cb_t cb; void *arg; };
static struct timer timers[4];
static int ntimers;
static int seen;
static void add(cb_t cb, void *arg) {
  timers[ntimers].cb = cb;
  timers[ntimers].arg = arg;
  ntimers++;
}
static void fire(void) {
  for (int i = 0; i < ntimers; i++) timers[i].cb(timers[i].arg);
}
static void show(void *p) { seen += *(int *)p; } // BUG: use-after-free // TRAP: object
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(show, x);
  free(x);
  fire(); // NOT-PROVEN: temporal
  return seen == 0;
}
