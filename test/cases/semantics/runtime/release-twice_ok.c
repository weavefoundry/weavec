// RFC 0032 §2.3, §3: the correct twin of release-twice_bug.c: without the error the connection is closed once.
// STAGE: S5
// The possible double free is still reported at the call: a boundary facet is not guarded
// (RFC 0032, Bugs deliberately not caught), so the case allows the warning.
// CLEAN
// ALLOW: double-free
// RUN-INPUT:
// ASAN
#include <stdio.h>
#include <stdlib.h>
struct conn { char *buf; int fd; };
static void conn_close(struct conn *c) {
  free(c->buf);
  free(c);
}
static int handle(struct conn *c, int err) {
  if (err) {
    conn_close(c);
    return -1;
  }
  return 0;
}
int main(int argc, char **argv) {
  struct conn *c = calloc(1, sizeof *c);
  (void)argv;
  if (!c) return 1;
  c->buf = malloc(16);
  int r = handle(c, argc > 1);
  conn_close(c);
  return r < 0;
}
