// CLEAN
// RUN-INPUT: -1
// RUN-INPUT: 2
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  int a[4] = {0, 1, 2, 3};
  int i = atoi(argv[1]);
  if (i >= 0 && i < 4) return a[i];
  return 0;
}
