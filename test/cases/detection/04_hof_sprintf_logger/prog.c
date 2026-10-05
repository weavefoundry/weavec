// RFC 0034 detection set, case 04 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: bob alexander_the_great_of_macedon
#include <stdio.h>
#include <stdlib.h>

struct logger {
  char *line;
  size_t cap;
  unsigned seq;
};

static void log_user(struct logger *lg, const char *user, int id) {
#ifdef FIX
  snprintf(lg->line, lg->cap, "user=%s id=%d", user, id);
#else
  sprintf(lg->line, "user=%s id=%d", user, id); // STOP
#endif
  printf("[%u] %s\n", lg->seq++, lg->line);
}

int main(int argc, char **argv) {
  struct logger lg = {0};
  lg.cap = 24;
  lg.line = malloc(lg.cap);
  if (!lg.line)
    return 1;
  for (int i = 1; i < argc; i++)
    log_user(&lg, argv[i], 1000 + i);
  free(lg.line);
  return 0;
}
