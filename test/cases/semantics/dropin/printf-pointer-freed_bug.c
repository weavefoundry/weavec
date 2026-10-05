// RFC 0033 §5: a `%s` argument that is a released heap string traps, also through fprintf.
// STAGE: S3
// RUN-INPUT: 1
// ASAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void drop(char *p, int really) { if (really) free(p); }
static void show(FILE *f, const char *name) {
  fprintf(f, "name=%s\n", name); // TRAP: object
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char *name = malloc(8);
  if (name == NULL) return 1;
  strcpy(name, "carol");
  drop(name, atoi(argv[1]));
  show(stdout, name); // BUG: use-after-free possible
  return 0;
}
