// RFC 0034 detection set, case 34 (invalid free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: '   indented' plain
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_name(const char *src) {
  char *s = malloc(strlen(src) + 1);
  if (!s)
    return NULL;
  strcpy(s, src);
  return s;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *name = read_name(argv[i]);
    if (!name)
      return 1;
#ifdef FIX
    char *start = name;
    while (isspace((unsigned char)*start))
      start++;
    printf("name: '%s'\n", start);
    free(name);
#else
    while (isspace((unsigned char)*name))
      name++; /* skips leading blanks in place */
    printf("name: '%s'\n", name);
    free(name); // STOP
#endif
  }
  return 0;
}
