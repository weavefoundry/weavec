// RFC 0032 §5: the correct twin of global-static-local_bug.c: the last element is inside 'table'.
// STAGE: S4
// CLEAN
// RUN-INPUT: 3
// ASAN
#include <stdlib.h>
struct view { const int *p; };
static const int *squares(void) {
  static int table[4] = {0, 1, 4, 9};
  return table;
}
static int at(const struct view *v, int i) {
  return v->p[i]; // GUARDED: spatial
}
int main(int argc, char **argv) {
  struct view v = {squares()};
  if (argc < 2) return 1;
  return at(&v, atoi(argv[1])) != 9;
}
