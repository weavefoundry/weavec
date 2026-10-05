// RFC 0034 detection set, case 38 (use after return): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: -v 3
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct options {
  int verbose;
  int level;
  char name[16];
};

static struct options *g_opts;
#ifdef FIX
static struct options g_store;
#endif

static void configure(int argc, char **argv) {
#ifdef FIX
  struct options *o = &g_store;
#else
  struct options local;
  struct options *o = &local;
#endif
  memset(o, 0, sizeof *o);
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0)
      o->verbose = 1;
    else
      o->level = atoi(argv[i]);
  }
  snprintf(o->name, sizeof o->name, "cfg%d", o->level);
  g_opts = o; /* published for the rest of the program */
}

static int churn(int seed) {
  volatile char junk[128];
  for (int i = 0; i < 128; i++)
    junk[i] = (char)(seed + i);
  return junk[seed & 127];
}

static void report(void) {
  printf("%s: level=%d verbose=%d\n", g_opts->name, g_opts->level, g_opts->verbose); // STOP // MISS: the dead frame's bytes are a live frame again when the guard looks, so it passes
}

int main(int argc, char **argv) {
  configure(argc, argv);
  int c = churn(argc);
  report();
  return c == 1000;
}
