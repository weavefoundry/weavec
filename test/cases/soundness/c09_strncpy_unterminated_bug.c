// RUN-INPUT: abcdefgh
// ASAN
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  char dst[4];
  strncpy(dst, argv[1], sizeof dst);
  return (int)strlen(dst); // BUG: out-of-bounds // TRAP: len
}
