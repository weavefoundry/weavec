// RFC 0033 §6.2: C library allocations are released through the arena like any other block.
// STAGE: S4
// CLEAN
// RUN-INPUT: 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char *a = strdup("alpha");
  char *line = NULL;
  size_t cap = 0;
  FILE *f = fopen("/dev/null", "r");
  if (a == NULL || f == NULL) return 1;
  ssize_t n = getline(&line, &cap, f);
  fclose(f);
  char *b = realloc(a, 64);
  if (b == NULL) { free(a); return 1; }
  strcat(b, "-beta");
  int r = (int)strlen(b) + (n < 0 ? 0 : 1);
  free(b);
  free(line);
  return r == 10 ? 0 : 1;
}
