// RFC 0034 detection set, case 56 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// UNITS: sbuf.c
// RUN-INPUT: alpha beta gamma delta epsilon
#include "sbuf.h"
#include <stdio.h>

int main(int argc, char **argv) {
  struct sbuf out;
  if (sbuf_init(&out, 16))
    return 1;
  for (int i = 1; i < argc; i++) {
    if (sbuf_append(&out, argv[i]) || sbuf_append(&out, ","))
      return 1;
  }
  printf("%s\n", out.data);
  sbuf_free(&out);
  return 0;
}
