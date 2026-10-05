// RFC 0034 detection set, case 31 (double free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: short 0123456789012345678901234567890123456789012345678901234567890123456789
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_MAX 64

static int load_record(const char *src, char **out) {
  char *buf = malloc(REC_MAX);
  if (!buf)
    return -1;
  *out = buf;
  if (strlen(src) >= REC_MAX) {
    free(buf);
#ifdef FIX
    *out = NULL;
#endif
    return -1;
  }
  strcpy(buf, src);
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *rec = NULL;
    if (load_record(argv[i], &rec) != 0)
      fprintf(stdout, "record %d too long\n", i);
    else
      printf("record %d: %s\n", i, rec);
    free(rec); // STOP
  }
  return 0;
}
