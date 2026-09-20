// CLEAN
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  int n = atoi(argv[1]);
  if (n <= 0 || n > 1024) return 1;
  char vla[n];
  vla[n - 1] = 0;
  return vla[n - 1];
}
