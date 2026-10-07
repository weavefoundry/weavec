// RFC 0030 §7.4: the twin of alias-cursor-several-arrays_ok.c whose cursor
// walks one element past the second array. The guard on the access stops it
// (in the array's own last granule: 24 bytes of a 16-byte-aligned local).
// RUN-INPUT:
// ASAN
#include <string.h>
int main(void) {
  const char *first[] = {"1", "2", "3"};
  const char *second[] = {"a", "b", "c"};
  const char **all[] = {first, second};
  int total = 0;
  for (unsigned s = 0; s < 2; ++s) {
    const char **cursor = all[s];
    for (unsigned i = 0; i < 3 + s; ++i) {
      total += (int)strlen(*cursor); // BUG: out-of-bounds // TRAP
      cursor++;
    }
  }
  return total;
}
