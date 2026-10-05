// RFC 0034 section 6.3 (partial unmapping): a variant of
// munmap-partial-range.c with constant offsets. Unmapping the trailing and
// leading 64 KiB of a 192 KiB mapping (a multiple of every supported page
// size) releases neither the mapping nor anything definitely, munmap takes a
// pointer anywhere in the mapping, and the middle stays usable. What remains
// are possible findings, hence the ALLOW.
// CLEAN
// ALLOW: double-free use-after-free
#include <stdio.h>
#include <sys/mman.h>

enum { Chunk = 1 << 16 };

int main(void) {
  unsigned char *base = mmap(NULL, 3 * Chunk, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
  if (base == MAP_FAILED)
    return 1;
  munmap(base + 2 * Chunk, Chunk);
  munmap(base, Chunk);
  base[Chunk] = 42;
  printf("%d\n", base[Chunk]);
  munmap(base + Chunk, Chunk);
  return 0;
}
