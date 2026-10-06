// RFC 0034 section 6.3 (values passed as values): a variant of
// munmap-hint-address.c. The old address of an unmapped region is passed
// straight to mmap as its address hint, to madvise (a range, not an object;
// it fails with ENOMEM), and to a unit function that only tests it for truth;
// none of them accesses the released object, so none is a use after free.
// CLEAN
#include <stdio.h>
#include <sys/mman.h>

static int remembered(const void *hint) {
  if (!hint)
    return 0;
  return hint ? 1 : 0;
}

int main(void) {
  size_t len = 1 << 16;
  char *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                   -1, 0);
  if (map == MAP_FAILED)
    return 1;
  map[0] = 'x';
  munmap(map, len);
  int seen = remembered(map);
  (void)madvise(map, len, MADV_DONTNEED);
  char *again = mmap(map, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                     -1, 0);
  if (again == MAP_FAILED)
    return 1;
  again[1] = 'y';
  printf("ok %c %d\n", again[1], seen);
  munmap(again, len);
  return 0;
}
