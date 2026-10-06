// RFC 0034 detection set, case 20 (global overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: /opt/tools/bin/my-very-long-program-name
#include <stdio.h>
#include <string.h>

static char g_progname[16];
static int g_verbose;

static void set_progname(const char *path) {
  const char *s = strrchr(path, '/');
  s = s ? s + 1 : path;
#ifdef FIX
  snprintf(g_progname, sizeof g_progname, "%s", s);
#else
  strcpy(g_progname, s); // STOP
#endif
}

static void warn(const char *msg) { fprintf(stdout, "%s: %s (verbose=%d)\n", g_progname, msg, g_verbose); }

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  set_progname(argv[1]);
  g_verbose = argc > 2;
  warn("starting");
  return 0;
}
