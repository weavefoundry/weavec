// RFC 0034 detection set, case 45 (overlapping copy): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: ./././src/lib/module/file.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strip_dot_slash(char *path) {
  while (path[0] == '.' && path[1] == '/') {
#ifdef FIX
    memmove(path, path + 2, strlen(path + 2) + 1);
#else
    strcpy(path, path + 2); // STOP
#endif
  }
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *p = strdup(argv[i]);
    if (!p)
      return 1;
    strip_dot_slash(p);
    printf("%s\n", p);
    free(p);
  }
  return 0;
}
