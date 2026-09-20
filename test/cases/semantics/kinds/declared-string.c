// RFC 0030 §7.2 (WEAVEC_STRING): a NUL-terminated requirement is checked at every call.
// STAGE: S6
// 's' is NulTerminated. At the call the requirement is checked against the exact extent of
// 'b' as __weavec_strnlen(b, 4) < 4 (§10.3 rule 2), a length check with the len template.
// With an argument the driver overwrites the terminator.
// RUN-INPUT: x
// ASAN
#include <stddef.h>
#include "../Inputs/rfc0030.h"

static size_t count_x(const char *WEAVEC_STRING s) {
  size_t n = 0;
  for (; *s; s++)
    if (*s == 'x') n++;
  return n;
}

int main(int argc, char **argv) {
  char b[4] = {'a', 'x', 'c', 0};
  (void)argv;
  if (argc > 1) b[3] = 'd';
  return (int)count_x(b); // BUG: out-of-bounds // TRAP: len
}
