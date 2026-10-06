// RFC 0034 detection set, case 19 (global overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: < stdin.txt
#include <stdio.h>

#define NBUCKETS 10

static unsigned histogram[NBUCKETS];
static unsigned total;

static void record(int score) {
#ifdef FIX
  int bucket = score >= 100 ? NBUCKETS - 1 : score / 10;
#else
  int bucket = score / 10; /* 100 maps to bucket 10 */
#endif
  if (bucket < 0)
    return;
  histogram[bucket]++; // STOP
  total++;
}

int main(void) {
  int s;
  while (scanf("%d", &s) == 1)
    record(s);
  for (int i = 0; i < NBUCKETS; i++)
    printf("%3d-%3d: %u\n", i * 10, i * 10 + 9, histogram[i]);
  printf("total %u\n", total);
  return 0;
}
