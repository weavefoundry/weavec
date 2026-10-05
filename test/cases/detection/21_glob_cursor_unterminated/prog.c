// RFC 0034 detection set, case 21 (global overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: ABCDEFGHIJ
#include <stdio.h>
#include <string.h>

static char g_tag[8];
static char g_motd[32] = "welcome to the system";

static void set_tag(const char *s) {
#ifdef FIX
  strncpy(g_tag, s, sizeof g_tag - 1);
  g_tag[sizeof g_tag - 1] = '\0';
#else
  strncpy(g_tag, s, sizeof g_tag); /* no terminator when s is long */
#endif
}

static size_t tag_len(void) {
  size_t n = 0;
  const char *p = g_tag;
  while (*p++) // STOP
    n++;
  return n;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  set_tag(argv[1]);
  printf("tag length %zu; %s\n", tag_len(), g_motd);
  return 0;
}
