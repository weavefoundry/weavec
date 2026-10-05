// RFC 0034 detection set, case 59 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 16 < stdin.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct blob {
  char *data;
  size_t len;
};

static int blob_read(struct blob *b, FILE *f, size_t want) {
#ifdef FIX
  b->data = malloc(want + 1);
#else
  b->data = malloc(want);
#endif
  if (!b->data)
    return -1;
  b->len = fread(b->data, 1, want, f);
  b->data[b->len] = '\0'; // STOP
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct blob b;
  if (blob_read(&b, stdin, strtoul(argv[1], NULL, 10)) != 0)
    return 1;
  printf("read %zu bytes, %zu chars before NUL\n", b.len, strlen(b.data));
  free(b.data);
  return 0;
}
