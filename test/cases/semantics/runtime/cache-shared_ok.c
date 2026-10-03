// RFC 0032 §13: the correct twin of cache-shared_bug.c: the last element passes both guards.
// STAGE: S8
// CLEAN
// RUN-INPUT: 15
// RUN-INPUT: 0
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
    total += p[k].a; // GUARDED: spatial
  }
  return total;
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  Table = calloc(16, sizeof *Table);
  if (!Table) return 2;
  return probe(atol(argv[1])) != 0;
}
