// RFC 0034 detection set, case 22 (global overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 2 0
#include <stdio.h>
#include <stdlib.h>

static int g_retry_limit[4] = {1, 3, 5, 10};
static int g_backoff_ms[4] = {10, 100, 1000, 5000};

/* levels are 1-based in the config file */
static int retries_for(int level) {
#ifdef FIX
  if (level < 1 || level > 4)
    return 0;
#endif
  return g_backoff_ms[level - 1] / 10; // STOP
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    printf("level %s -> %d\n", argv[i], retries_for(atoi(argv[i])));
  g_retry_limit[0] = argc;
  return g_retry_limit[0] == 0;
}
