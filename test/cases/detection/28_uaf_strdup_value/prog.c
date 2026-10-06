// RFC 0034 detection set, case 28 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: name=weavec level=3
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* returns the value of a "key=value" line, or NULL */
static char *config_value(const char *line) {
  char *copy = strdup(line);
  if (!copy)
    return NULL;
  char *eq = strchr(copy, '=');
  if (!eq) {
    free(copy);
    return NULL;
  }
#ifdef FIX
  char *v = strdup(eq + 1);
#else
  char *v = eq + 1;
#endif
  free(copy);
  return v;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *v = config_value(argv[i]);
    if (!v)
      continue;
    printf("value=%s len=%zu\n", v, strlen(v)); // STOP
#ifdef FIX
    free(v);
#endif
  }
  return 0;
}
