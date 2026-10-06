// RFC 0034 detection set, case 05 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: /usr/local include share
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *path_join(const char *dir, const char *file) {
#ifdef FIX
  char *out = malloc(strlen(dir) + strlen(file) + 2);
#else
  char *out = malloc(strlen(dir) + strlen(file) + 1); /* no room for '/' */
#endif
  if (!out)
    return NULL;
  strcpy(out, dir);
  strcat(out, "/");
  strcat(out, file); // STOP
  return out;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  for (int i = 2; i < argc; i++) {
    char *p = path_join(argv[1], argv[i]);
    if (!p)
      return 1;
    printf("%s (%zu)\n", p, strlen(p));
    free(p);
  }
  return 0;
}
