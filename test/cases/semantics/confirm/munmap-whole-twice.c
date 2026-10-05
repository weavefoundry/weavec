// RFC 0034 section 6.3 (partial unmapping): the other side of
// munmap-partial-range.c. A munmap whose range covers the whole mapping from
// its start still releases it, so unmapping it again, or using it after, is
// a definite error. (A failed mmap returns MAP_FAILED, whose munmap fails
// with EINVAL.)
#include <stddef.h>
#include <sys/mman.h>

void twice(size_t len) {
  char *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                   -1, 0);
  munmap(map, len);
  munmap(map, len); // BUG: double-free definite
}

int after(void) {
  char *map = mmap(NULL, 4096 * 4, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
  munmap(map, 4096 * 4);
  return map[0]; // BUG: use-after-free definite
}
