// RFC 0030 §11: zero-initialisation covers the allocations it rewrites only.
// STAGE: S7
// `getaddrinfo` returns a list the C library allocated and filled; its
// `ai_addr` fields are the library's, not zeros (hiredis's `net.c`). Only a
// row the zero-initialisation wrapper covers (`malloc`, `calloc`, ...) makes
// an object whose unwritten bytes read as zero; this one's read as unknown, so
// passing `ai_addr` to `connect` is no null dereference.
// The early return when `getaddrinfo` fails is reported as a possible leak of
// its result: the table cannot yet say that a non-zero result means nothing was
// stored (`null-on-failure` names no result class), so the leak is allowed.
// CLEAN
// ALLOW: leak
// TOOL
#include <netdb.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>

int dial(const char *host, const char *port) {
  struct addrinfo hints = {0};
  struct addrinfo *info = NULL;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &info) != 0)
    return -1;
  int fd = -1;
  for (struct addrinfo *p = info; p != NULL; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0)
      continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(info);
  return fd;
}
