// RFC 0039 §9, C++ detection: a C library call past a block, in a scope with a destructor (an invoke). -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <string>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
int main() {
  std::string keep(40, 'k');
  char *buffer = static_cast<char *>(malloc(8));
  int fd = open("/dev/zero", O_RDONLY);
#ifdef FIX
  const size_t n = 8;
#else
  const size_t n = 64;
#endif
  long got = read(fd, buffer, n); // STOP
  return got < 0 || keep.size() != 40;
}
