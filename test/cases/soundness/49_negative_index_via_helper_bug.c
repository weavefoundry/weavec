// Helper indexes with a signed parameter; caller passes a negative value from input.
// RUN-INPUT: -1
// ASAN
#include <stdlib.h>
static int table[4] = {1, 2, 3, 4};
static int lookup(int i) { return table[i]; } // BUG: out-of-bounds // TRAP: index
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  return lookup(atoi(argv[1]));
}
