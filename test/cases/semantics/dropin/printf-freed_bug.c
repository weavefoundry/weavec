// RFC 0033 §5: a `%s` argument of printf is guarded: a string inside a released object traps.
// STAGE: S3
// RUN-INPUT: 1
// ASAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct session { char user[16]; int id; };
static void audit(struct session *s, int ended) {
  if (ended) free(s);
  printf("audit: %s\n", s->user); // BUG: use-after-free possible // TRAP: object
}
int main(int argc, char **argv) {
  struct session *s = calloc(1, sizeof *s);
  if (s == NULL || argc < 2) return 1;
  strcpy(s->user, "bob");
  audit(s, atoi(argv[1]));
  return 0;
}
