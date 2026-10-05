// RFC 0034 detection set, case 48 (libc-allocated misuse): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: libnetworking_core_networking_core_networking_core_networking_core_networking_core_networking_core_networking_core_mmmmmmmm 12
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *object_name(const char *base, int version) {
  char *s = NULL;
#ifdef FIX
  int n = asprintf(&s, "%s-v%d.o", base, version);
  if (n < 0)
    return NULL;
#else
  int n = asprintf(&s, "%s-v%d", base, version);
  if (n < 0)
    return NULL;
  memcpy(s + n, ".o", 3); // STOP
#endif
  return s;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  char *name = object_name(argv[1], atoi(argv[2]));
  if (!name)
    return 1;
  printf("%s\n", name);
  free(name);
  return 0;
}
