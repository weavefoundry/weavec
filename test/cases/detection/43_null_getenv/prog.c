// RFC 0034 detection set, case 43 (null dereference): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: WEAVEC_EVAL_UNSET_VARIABLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t path_depth(const char *var) {
  const char *dir = getenv(var);
#ifdef FIX
  if (!dir)
    return 0;
#endif
  size_t depth = 0;
  size_t n = strlen(dir); // STOP
  for (size_t i = 0; i < n; i++)
    if (dir[i] == '/')
      depth++;
  return depth;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    printf("%s has depth %zu\n", argv[i], path_depth(argv[i]));
  return 0;
}
