// RFC 0037 §5: getsockname writes as many bytes as the length it is given
// says; its wrapper checks the buffer against it.
// RUN-INPUT:
// ASAN
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>

int main(void) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  char *small = malloc(4);
  socklen_t length = sizeof(struct sockaddr_in);
  return getsockname(fd, (struct sockaddr *)small, &length); // TRAP: heap-buffer-overflow
}
