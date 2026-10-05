// RFC 0034 detection set, case 60 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// UNITS: msg.c
// RUN-INPUT: 5 hello 9
#include "msg.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    struct msg *m = msg_new(argv[i], atoi(argv[i]) > 0 ? 1 : 0);
    if (!m)
      return 1;
#ifdef FIX
    int prio = m->prio;
    int ok = msg_send(m);
    if (ok && prio > 0)
      printf("urgent message sent\n");
#else
    int ok = msg_send(m);
    if (ok && m->prio > 0) // STOP
      printf("urgent message sent\n");
#endif
  }
  return 0;
}
