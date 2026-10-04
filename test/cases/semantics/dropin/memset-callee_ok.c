// RFC 0033 §1: a callee's memset over its whole parameter needs exactly the bytes it sets;
// a store's own width is used, not the element type's (zstd's HUF_readStats).
// STAGE: S1
// CLEAN
// RUN-INPUT:
#include <string.h>
static void clear_ints(int *p) { memset(p, 0, 4 * sizeof(int)); }
static void clear_shorts(short *p) { memset(p, 0, 8 * sizeof(short)); }
static void clear_longs(long *p) { memset(p, 0, 2 * sizeof(long)); }
int main(void) {
  int a[4] = {1, 2, 3, 4};
  short s[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  long l[2] = {1, 2};
  clear_ints(a);
  clear_shorts(s);
  clear_longs(l);
  return a[3] + s[7] + (int)l[1];
}
