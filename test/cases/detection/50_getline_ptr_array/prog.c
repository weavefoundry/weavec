// RFC 0034 detection set, case 50 (libc-allocated misuse): one of the 61 blind bug programs
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
  size_t cap = 4, n = 0;
  char **lines = malloc(cap * sizeof *lines);
  if (!lines)
    return 1;
  char *line = NULL;
  size_t lcap = 0;
  ssize_t len;
  while ((len = getline(&line, &lcap, stdin)) > 0) {
    if (line[len - 1] == '\n')
      line[len - 1] = '\0';
#ifdef FIX
    if (n == cap) {
      char **nl = realloc(lines, cap * 2 * sizeof *nl);
      if (!nl)
        return 1;
      lines = nl;
      cap *= 2;
    }
#endif
    lines[n++] = strdup(line); // STOP
  }
  free(line);
  for (size_t i = n; i > 0; i--)
    printf("%zu: %s\n", i, lines[i - 1]);
  for (size_t i = 0; i < n; i++)
    free(lines[i]);
  free(lines);
  return 0;
}
