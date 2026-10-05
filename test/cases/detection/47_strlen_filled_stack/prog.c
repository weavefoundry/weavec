// RFC 0034 detection set, case 47 (unterminated string): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: SECTIONHEADER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *normalize_tag(const char *in) {
  char tag[8];
#ifdef FIX
  strncpy(tag, in, sizeof tag - 1);
  tag[sizeof tag - 1] = '\0';
#else
  strncpy(tag, in, sizeof tag);
#endif
  size_t n = strlen(tag); // STOP
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i++)
    out[i] = (char)(tag[i] | 0x20);
  out[n] = '\0';
  return out;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *t = normalize_tag(argv[i]);
    if (!t)
      return 1;
    printf("tag=%s\n", t);
    free(t);
  }
  return 0;
}
