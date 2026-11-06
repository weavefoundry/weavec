// RFC 0038 amendment 16: calls the C library allows on arguments a row
// once read too much of. mbstowcs converts at most its count of wide
// characters, so its source need not be terminated (CPython converts one
// byte so); setsockopt takes a null value with a length (Linux's AF_ALG
// authentication size), which the kernel checks. Neither stops.
// CLEAN
// RUN-INPUT:
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wchar.h>

static volatile size_t sink;

int main(void) {
  char *one = malloc(1);
  wchar_t wide[1];
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  one[0] = 'A';
  sink = mbstowcs(wide, one, 1);
  if (fd >= 0) {
    sink = (size_t)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, NULL, 4);
    (void)close(fd);
  }
  free(one);
  return 0;
}
