// RFC 0032 §5: a global the unit defines is a tracked object: an index past it through a pointer of unknown extent traps.
// STAGE: S4
// RUN-INPUT: 8
// ASAN
#include <stdlib.h>
static int table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
struct view { const int *p; };
static int at(const struct view *v, int i) {
  return v->p[i]; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
int main(int argc, char **argv) {
  struct view v = {table};
  if (argc < 2) return 1;
  return at(&v, atoi(argv[1])) == 0;
}
