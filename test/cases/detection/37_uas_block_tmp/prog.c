// RFC 0034 detection set, case 37 (use after scope): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 5 2
#include <stdio.h>
#include <stdlib.h>

static int consume(const int *v, int idx) { return v ? v[idx] : -1; }

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  int n = atoi(argv[1]);
  int idx = atoi(argv[2]) & 3;
  const int *weights = NULL;
#ifdef FIX
  int tmp[4];
#endif
  if (n > 0) {
#ifndef FIX
    int tmp[4];
#endif
    for (int i = 0; i < 4; i++)
      tmp[i] = n * (i + 1);
    weights = tmp;
  }
  int other[16];
  for (int i = 0; i < 16; i++)
    other[i] = -i;
  printf("weight=%d other=%d\n", consume(weights, idx), other[idx]); // STOP
  return 0;
}
