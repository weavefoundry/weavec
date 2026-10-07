// RFC 0032 §5: a static local is a tracked object like a global: an index past it through a pointer of unknown extent traps.
// STAGE: S4
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>
struct view { const int *p; };
static const int *squares(void) {
  static int table[4] = {0, 1, 4, 9};
  return table;
}
static int at(const struct view *v, int i) {
  return v->p[i]; // BUG: out-of-bounds // TRAP
}
int main(int argc, char **argv) {
  struct view v = {squares()};
  if (argc < 2) return 1;
  return at(&v, atoi(argv[1])) == 0;
}
