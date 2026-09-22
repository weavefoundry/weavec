// RFC 0030 §8.3: zero-length read, write, memmove, memset, memcmp, snprintf and strncpy accept null.
// STAGE: S4
// Each row marks its pointers null-if-zero, so a null pointer with a length of zero (known
// only at run time here) passes the zero-length form of the nonnull check. No error, no
// trap.
// CLEAN
// ASAN
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  size_t n = (size_t)argc - 1;
  char *none = NULL;
  (void)argv;
  if (read(0, none, n) < 0) return 1;
  if (write(1, none, n) < 0) return 1;
  memmove(none, none, n);
  memset(none, 0, n);
  int c = memcmp(none, none, n);
  int w = snprintf(none, n, "%d", 42);
  strncpy(none, "abc", n);
  return c == 0 && w == 2 ? 0 : 1;
}
