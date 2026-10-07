// RFC 0032 §2.3, §3: a release is validated against the allocator's own state: the second release of a block traps.
// STAGE: S5
// 'handle' closes the connection on an error and its caller closes it again.
// RUN-INPUT: x
// ASAN
#include <stdio.h>
#include <stdlib.h>
struct conn { char *buf; int fd; };
static void conn_close(struct conn *c) {
  free(c->buf); // TRAP
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
  conn_close(c); // BUG: double-free possible
  return r < 0;
}
