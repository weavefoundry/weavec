// RFC 0034 detection set, case 26 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// UNITS: conn.c
// RUN-INPUT: HELO MAIL QUIT
#include "conn.h"
#include <stdio.h>
#include <string.h>

static int handle_line(struct conn *c, const char *line) {
  if (strcmp(line, "QUIT") == 0) {
    conn_close(c);
    return 1;
  }
  conn_feed(c, line);
  conn_feed(c, ";");
  return 0;
}

int main(int argc, char **argv) {
  struct conn *c = conn_open(3, 64);
  if (!c)
    return 1;
  int closed = 0;
  for (int i = 1; i < argc && !closed; i++)
    closed = handle_line(c, argv[i]);
#ifdef FIX
  if (!closed)
#endif
    printf("fd %d: %zu bytes buffered: %s\n", c->fd, c->rlen, c->rbuf); // STOP
#ifdef FIX
  if (!closed)
    conn_close(c);
#endif
  return 0;
}
