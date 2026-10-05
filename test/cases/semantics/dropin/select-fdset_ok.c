// RFC 0033 §3: select reads and writes only the fd_set words that cover nfds descriptors, so
// a set allocated for exactly those words is enough (libuv's stream.c select thread).
// STAGE: S2
// CLEAN
// RUN-INPUT:
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include <stdlib.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
int main(void) {
  int fds[2];
  if (pipe(fds) != 0) return 1;
  int nfds = fds[1] + 1;
  size_t words = (size_t)(nfds + NFDBITS - 1) / NFDBITS;
  fd_set *readable = calloc(words, sizeof(fd_mask));
  if (readable == NULL) return 1;
  if (write(fds[1], "x", 1) != 1) return 1;
  FD_SET(fds[0], readable);
  struct timeval none = {0, 0};
  int r = select(nfds, readable, NULL, NULL, &none);
  int ok = r == 1 && FD_ISSET(fds[0], readable);
  free(readable);
  close(fds[0]);
  close(fds[1]);
  return ok ? 0 : 1;
}
