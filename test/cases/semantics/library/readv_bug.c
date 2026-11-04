// RFC 0037 §5: readv's wrapper checks each buffer its iovec array names.
// RUN-INPUT:
// ASAN
#include <fcntl.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  char *small = malloc(8);
  struct iovec v = {small, 64};
  const int fd = open("/dev/zero", O_RDONLY);
  return (int)readv(fd, &v, 1); // TRAP: heap-buffer-overflow
}
