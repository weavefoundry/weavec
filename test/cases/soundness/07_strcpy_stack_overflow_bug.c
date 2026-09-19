// Stack overflow: strcpy / sprintf of argv into fixed buffer.
// RUN-INPUT: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
// ASAN
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 0;
  strcpy(buf, argv[1]); // BUG: out-of-bounds // TRAP: len
  return buf[0];
}
