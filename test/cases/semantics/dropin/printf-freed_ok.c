// RFC 0033 §5: format arguments that are live pass their guards.
// STAGE: S3
// CLEAN
// RUN-INPUT: 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct session { char user[16]; int id; };
static void audit(struct session *s, int verbose) {
  char *copy = malloc(8);
  if (copy == NULL) return;
  strcpy(copy, "x");
  printf("audit: %s %s %d %p\n", s->user, copy, verbose, (void *)s);
  free(copy);
}
int main(int argc, char **argv) {
  struct session *s = calloc(1, sizeof *s);
  if (s == NULL || argc < 2) return 1;
  strcpy(s->user, "bob");
  audit(s, atoi(argv[1]));
  free(s);
  return 0;
}
