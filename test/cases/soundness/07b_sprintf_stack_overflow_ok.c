// CLEAN
// RUN-INPUT: AAAAAAAAAAAAAAAA
// ASAN
#include <stdio.h>
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 0;
  snprintf(buf, sizeof buf, "x=%s", argv[1]);
  return buf[0];
}
