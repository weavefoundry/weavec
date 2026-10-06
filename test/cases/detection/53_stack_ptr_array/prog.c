// RFC 0034 detection set, case 53 (stack overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: -v a.c b.c c.c d.c e.c f.c
#include <stdio.h>

#define MAX_FILES 4

static int process(const char **files, int n, int verbose) {
  for (int i = 0; i < n; i++)
    printf("%s%s\n", verbose ? "processing " : "", files[i]);
  return n;
}

int main(int argc, char **argv) {
  const char *files[MAX_FILES];
  int nfiles = 0;
  int verbose = 0;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      verbose = 1;
      continue;
    }
#ifdef FIX
    if (nfiles == MAX_FILES) {
      fprintf(stderr, "too many files, ignoring %s\n", argv[i]);
      continue;
    }
#endif
    files[nfiles++] = argv[i]; // STOP
  }
  return process(files, nfiles, verbose) == 0;
}
