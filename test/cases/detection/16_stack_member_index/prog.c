// RFC 0034 detection set, case 16 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 0 80 1 443 5 9999
#include <stdio.h>
#include <stdlib.h>

struct config {
  int ports[4];
  int nports;
  int timeout;
};

static void set_port(struct config *cfg, int idx, int port) {
#ifdef FIX
  if (idx < 0 || idx >= 4)
    return;
#endif
  cfg->ports[idx] = port; // STOP
  if (idx >= cfg->nports)
    cfg->nports = idx + 1;
}

int main(int argc, char **argv) {
  struct config cfg = {{0}, 0, 30};
  for (int i = 1; i + 1 < argc; i += 2)
    set_port(&cfg, atoi(argv[i]), atoi(argv[i + 1]));
  printf("nports=%d timeout=%d\n", cfg.nports, cfg.timeout);
  return 0;
}
