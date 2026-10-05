// RFC 0034 detection set, case 14 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 3 9 2 7 4 8 1 6
#include <stdio.h>
#include <stdlib.h>

#define NSAMPLES 8

static int peak(const int *s, int n) {
  int best = s[0];
#ifdef FIX
  for (int i = 1; i < n; i++)
#else
  for (int i = 1; i <= n; i++)
#endif
    if (s[i] > best) // STOP
      best = s[i];
  return best;
}

int main(int argc, char **argv) {
  int samples[NSAMPLES];
  int n = 0;
  for (int i = 1; i < argc && n < NSAMPLES; i++)
    samples[n++] = atoi(argv[i]);
  if (n == 0)
    return 2;
  int guard_val = argc * 1000;
  printf("peak=%d (%d)\n", peak(samples, n), guard_val); // STOP // (requirement of peak())
  return 0;
}
