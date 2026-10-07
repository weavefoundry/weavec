// RFC 0032 §5: the correct twin of global-view_bug.c: the last element is inside 'table'.
// STAGE: S4
// CLEAN
// RUN-INPUT: 7
// ASAN
#include <stdlib.h>
static int table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
struct view { const int *p; };
static int at(const struct view *v, int i) {
  return v->p[i];
}
int main(int argc, char **argv) {
  struct view v = {table};
  if (argc < 2) return 1;
  return at(&v, atoi(argv[1])) != 8;
}
