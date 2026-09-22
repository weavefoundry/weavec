// CLEAN
// RUN-INPUT: %s%s%s%s%s%s%s%s
// ASAN
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  printf("%s", argv[1]);
  printf("%s %s\n", argv[1], argv[1]);
  return 0;
}
