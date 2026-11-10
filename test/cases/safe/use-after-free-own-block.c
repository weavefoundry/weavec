// RFC 0041 §6 (S3): a use after free in a safe function reads the released
// block's own bytes, never another object's, and is not reported.
// CLEAN
// FLAGS: -O1
#include <weavec.h>
#include <stdlib.h>

WEAVEC_SAFE int stale(void) {
  int *a = calloc(4, sizeof *a);
  a[0] = 42;
  free(a);
  int *b = calloc(4, sizeof *b); // never a's block while a points to it
  b[0] = 7;
  int r = a[0] == 7;
  free(b);
  return r;
}

int main(void) { return stale(); }
