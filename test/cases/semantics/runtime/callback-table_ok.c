// RFC 0032 §3: the correct twin of callback-table_bug.c: the timers fire before the argument is freed.
// STAGE: S3
// CLEAN
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
static void show(void *p) { seen += *(int *)p; }
int main(void) {
  int *x = malloc(sizeof *x);
  if (!x) return 1;
  *x = 7;
  add(show, x);
  fire();
  ntimers = 0;
  timers[0].arg = NULL;
  free(x);
  return seen != 7;
}
