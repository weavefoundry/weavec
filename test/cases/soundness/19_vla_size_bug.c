// VLA: size from input, off-by-one write, and non-positive size.
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  int n = atoi(argv[1]);
  char vla[n];
  vla[n] = 0; // BUG: out-of-bounds
  return vla[0];
}
