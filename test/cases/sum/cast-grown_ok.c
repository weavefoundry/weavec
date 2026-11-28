// RFC 0054 §2.2, amendment 22: the twin of cast-grown.c: a block `calloc`
// made for 6 elements and `realloc` grew to 7 through a `void *` is read at
// element 6; an `int` array converted from a `void *` is
// read at its last element; and the first struct of a block whose type
// begins with it is read.
// CLEAN
// FLAGS: -O2 -fweavec-safe
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct b { long n; long m; };
struct outer { struct b first; long rest[2]; };

__attribute__((noinline)) long grown(void) {
  struct b *WEAVEC_OWNED bs = calloc(6, sizeof *bs);
  if (bs == NULL)
    return -1;
  void *WEAVEC_OWNED g = realloc(bs, 7 * sizeof *bs);
  if (g == NULL) {
    free(bs);
    return -1;
  }
  struct b *WEAVEC_OWNED ds = g;
  ds[6].n = 42;
  long v = ds[6].n;
  free(ds);
  return v;
}

__attribute__((noinline)) long ints(void) {
  void *WEAVEC_OWNED g = calloc(5, sizeof(int));
  if (g == NULL)
    return -1;
  int *WEAVEC_OWNED xs = g;
  xs[4] = 7;
  long v = xs[4];
  free(xs);
  return v;
}

__attribute__((noinline)) long member(void) {
  struct outer *WEAVEC_OWNED os = calloc(4, sizeof *os);
  if (os == NULL)
    return -1;
  void *WEAVEC_OWNED g = os;
  struct b *WEAVEC_OWNED first = g;
  long v = first[0].m;
  free(first);
  return v;
}

WEAVEC_GUARDED int main(void) {
  printf("%ld %ld %ld\n", grown(), ints(), member());
  return 0;
}
