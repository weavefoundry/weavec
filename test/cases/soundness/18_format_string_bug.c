// Format string: user-controlled format, and missing argument.
// RUN-INPUT: %s%s%s%s%s%s%s%s
// No ASAN marker: what ASan sees depends on whatever the missing
// argument's slot holds, which differs between machines.
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  printf(argv[1]);
  printf("%s %s\n", argv[1]); // BUG: out-of-bounds
  return 0;
}
