// RFC 0034 detection set, case 29 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: < stdin.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  char *first = NULL;
  size_t count = 0, bytes = 0;
  while ((n = getline(&line, &cap, stdin)) > 0) {
    if (!first) {
#ifdef FIX
      first = strdup(line);
#else
      first = line; /* keeps getline's buffer, not a copy */
#endif
    }
    count++;
    bytes += (size_t)n;
  }
  free(line);
  if (first)
    printf("%zu lines, %zu bytes, first: %s", count, bytes, first); // STOP
#ifdef FIX
  free(first);
#endif
  return 0;
}
