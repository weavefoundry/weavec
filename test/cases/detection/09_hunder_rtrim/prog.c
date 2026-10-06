// RFC 0034 detection set, case 09 (heap underflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 'abc  ' '        '
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *rtrim(char *s) {
  size_t len = strlen(s);
#ifdef FIX
  while (len > 0 && isspace((unsigned char)s[len - 1]))
    len--;
#else
  while (isspace((unsigned char)s[len - 1])) // STOP
    len--;
#endif
  s[len] = '\0';
  return s;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *copy = malloc(strlen(argv[i]) + 1);
    if (!copy)
      return 1;
    strcpy(copy, argv[i]);
    printf("[%s]\n", rtrim(copy));
    free(copy);
  }
  return 0;
}
