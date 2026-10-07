// RFC 0032 §13 (range caches): a 'live' guard and an 'object' guard of one pointer share a cache
// entry, and the entry holds the object's bytes, not its allocator slot's. When this unit is
// compiled, 'hook' is unknown and may release the table, so 'p->a' gets a 'live' guard (the link
// later proves the facet, which is why no GUARDED marker pins it); 'p[k].a' is then checked
// against the 16 elements, although the block's slot has room for more.
// STAGE: S8
// RUN-INPUT: 16
// UNITS: Inputs/cache-shared-hook.c
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct pair *Table;
void hook(void);
static int probe(long k) {
  struct pair *p = Table;
  int total = 0;
  long j;
  for (j = 0; j < 2; j++) {
    hook();
    total += p->a;
    total += p[k].a; // BUG: out-of-bounds // TRAP
  }
  return total;
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  Table = calloc(16, sizeof *Table);
  if (!Table) return 2;
  return probe(atol(argv[1])) != 0;
}
