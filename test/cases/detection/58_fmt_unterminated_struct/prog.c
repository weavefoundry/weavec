// RFC 0034 detection set, case 58 (unterminated string): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: AB CDEF
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct record {
  char code[4];
};

static struct record *records_load(char **src, int n) {
  struct record *r = malloc((size_t)n * sizeof *r);
  if (!r)
    return NULL;
  for (int i = 0; i < n; i++) {
#ifdef FIX
    snprintf(r[i].code, sizeof r[i].code, "%s", src[i]);
#else
    strncpy(r[i].code, src[i], sizeof r[i].code); /* "ABCD" leaves no NUL */
#endif
  }
  return r;
}

int main(int argc, char **argv) {
  int n = argc - 1;
  if (n < 1)
    return 2;
  struct record *r = records_load(argv + 1, n);
  if (!r)
    return 1;
  printf("last code: %s\n", r[n - 1].code); // STOP
  free(r);
  return 0;
}
