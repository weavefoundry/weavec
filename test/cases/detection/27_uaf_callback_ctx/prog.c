// RFC 0034 detection set, case 27 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: tick tick done
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*handler_fn)(void *ctx, const char *ev);

struct listener {
  handler_fn fn;
  void *ctx;
};

struct job {
  char name[16];
  int runs;
};

static int on_event(void *ctx, const char *ev) {
  struct job *j = ctx;
  j->runs++;
  if (strcmp(ev, "done") == 0) {
    free(j); /* the job owns itself once it is done */
    return 1;
  }
  return 0;
}

static void dispatch(struct listener *l, const char *ev) {
  int finished = l->fn(l->ctx, ev);
#ifdef FIX
  if (finished) {
    printf("job finished\n");
    l->ctx = NULL;
    return;
  }
#else
  (void)finished;
#endif
  struct job *j = l->ctx;
  printf("%s: %s after %d runs\n", j->name, ev, j->runs); // STOP
}

int main(int argc, char **argv) {
  struct job *j = calloc(1, sizeof *j);
  if (!j)
    return 1;
  snprintf(j->name, sizeof j->name, "backup");
  struct listener l = {on_event, j};
  for (int i = 1; i < argc && l.ctx; i++)
    dispatch(&l, argv[i]);
  return 0;
}
