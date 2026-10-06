// RFC 0034 section 6.3 (rows that disagree), a variant of release-by-flag.c:
// open_it() returns a malloc'd buffer or a FILE stream and says which by a
// flag, so its two result rows are one new object with two release families.
// Applied together, the family is unknown: neither free() nor fclose() of the
// result is a definite mismatched release.
// CLEAN
// ALLOW: mismatched-release
// RUN-INPUT:
// RUN-INPUT: mem
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *open_it(const char *name, int *is_mem) {
  if (strcmp(name, "mem") == 0) {
    char *b = malloc(16);
    if (!b) abort();
    *is_mem = 1;
    return b;
  }
  FILE *f = fopen(name, "r");
  if (!f) abort();
  *is_mem = 0;
  return f;
}

int main(int argc, char **argv) {
  int is_mem = 0;
  void *h = open_it(argc > 1 ? argv[1] : "/dev/null", &is_mem);
  if (is_mem) { free(h); puts("mem"); return 0; }
  fclose(h);
  puts("file");
  return 0;
}
