// RFC 0034 detection set, case 18 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: very.long.hostname.example.com 8080
#include <stdio.h>
#include <stdlib.h>

#ifdef FIX
static void format_addr(char *out, size_t cap, const char *host, int port) {
  snprintf(out, cap, "%s:%d", host, port);
}
#else
static void format_addr(char *out, const char *host, int port) {
  sprintf(out, "%s:%d", host, port); // STOP
}
#endif

static int connect_to(const char *host, int port) {
  char addr[24];
#ifdef FIX
  format_addr(addr, sizeof addr, host, port);
#else
  format_addr(addr, host, port);
#endif
  printf("connecting to %s\n", addr);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  return connect_to(argv[1], atoi(argv[2]));
}
