// CLEAN
// RUN-INPUT: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
// RUN-INPUT: abc
// ASAN
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 0;
  if (strlen(argv[1]) >= sizeof buf) return 1;
  strcpy(buf, argv[1]);
  return buf[0];
}
