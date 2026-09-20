// RFC 0030 §7.2 (alloc_size): the result is Sized(arg), and a check enforces the declaration.
// STAGE: S6
// 'grab' is declared alloc_size(1) and defined in a unit WeaveC does not analyse, which
// allocates 16 bytes more than asked. The declared Sized(8) may be tighter than the
// allocation (§7.1): p[i] with an unknown 'i' is checked with the index template against 8,
// and the run's index 8 traps although it lies inside the real block.
// UNITS: Inputs/grab-impl.c
// RUN-INPUT: 8
#include <stdlib.h>

void *grab(size_t n) __attribute__((alloc_size(1)));

int main(int argc, char **argv) {
  char *p = grab(8);
  if (p == NULL) return 1;
  size_t i = argc > 1 ? (size_t)atoi(argv[1]) : 0;
  p[i] = 1; // TRAP: index
  return 0;
}
