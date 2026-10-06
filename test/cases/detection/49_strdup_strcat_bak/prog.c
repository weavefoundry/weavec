// RFC 0034 detection set, case 49 (libc-allocated misuse): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: config.ini /etc/hosts
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *backup_name(const char *path) {
#ifdef FIX
  size_t n = strlen(path);
  char *d = malloc(n + sizeof ".bak");
  if (!d)
    return NULL;
  memcpy(d, path, n + 1);
#else
  char *d = strdup(path);
  if (!d)
    return NULL;
#endif
  strcat(d, ".bak"); // STOP
  return d;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *b = backup_name(argv[i]);
    if (!b)
      return 1;
    printf("%s -> %s\n", argv[i], b);
    free(b);
  }
  return 0;
}
