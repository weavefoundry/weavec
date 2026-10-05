// RFC 0034 section 6.3: a false definite use after free the milestone found
// in LMDB's mdb_env_set_mapsize (build/eval-2026-10-04/repros/lmdb-1.c): the
// address of an unmapped region is passed to mmap as a hint and only
// compared, never dereferenced. An unconfirmed candidate may remain a
// warning (RFC 0034, Diagnostics); and the leak report the runner asks for
// (-Wweavec-leak) takes map_at's MAP_FAILED return for a mapping.
// CLEAN
// ALLOW: use-after-free leak
/* LMDB mdb_env_set_mapsize: after munmap, the old base address is passed
 * as an address hint to mmap (never dereferenced). weavec-cc reports
 * use-after-free and fails the build. */
#include <stdio.h>
#include <sys/mman.h>

static char *map_at(void *hint, size_t len) {
  char *p = mmap(hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return NULL;
  if (hint && p != hint) puts("moved"); /* only compares the address */
  return p;
}

int main(void) {
  size_t len = 1 << 16;
  char *map = map_at(NULL, len);
  if (!map) return 1;
  map[0] = 'x';
  void *old;
#ifndef NEGATIVE
  munmap(map, len);
  old = map;           /* dangling, used only as an address */
#else
  old = map;           /* negative: same hint, no munmap before it */
  munmap(map, len);
  old = NULL;
#endif
  map = map_at(old, len);
  if (!map) return 1;
  map[1] = 'y';
  printf("ok %c\n", map[1]);
  munmap(map, len);
  return 0;
}
