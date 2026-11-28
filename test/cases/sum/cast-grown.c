// RFC 0054 §2.2, amendment 22: a pointer converted from a `void *` is
// bounded by the objects of its type the runtime finds there, the rest of an
// array of the type: after `realloc` grows a block to 7 elements through a
// `void *`, a read of element 7 stops, as does a read past an `int` array
// converted the same way, and a read past one struct that is only the first
// member of the block's type.
// FLAGS: -O2 -fweavec-safe
// RUN-INPUT: grown
// RUN-INPUT: ints
// RUN-INPUT: member
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct b { long n; long m; };
struct outer { struct b first; long rest[2]; };

__attribute__((noinline)) long grown(int i) {
  struct b *WEAVEC_OWNED bs = calloc(6, sizeof *bs);
  if (bs == NULL)
    return -1;
  void *WEAVEC_OWNED g = realloc(bs, 7 * sizeof *bs);
  if (g == NULL) {
    free(bs);
    return -1;
  }
  struct b *WEAVEC_OWNED ds = g;
  long v = ds[i].n; // TRAP: pointer-out-of-bounds
  free(ds);
  return v;
}

__attribute__((noinline)) long ints(int i) {
  void *WEAVEC_OWNED g = calloc(5, sizeof(int));
  if (g == NULL)
    return -1;
  int *WEAVEC_OWNED xs = g;
  long v = xs[i]; // TRAP: pointer-out-of-bounds
  free(xs);
  return v;
}

__attribute__((noinline)) long member(int i) {
  struct outer *WEAVEC_OWNED os = calloc(4, sizeof *os);
  if (os == NULL)
    return -1;
  void *WEAVEC_OWNED g = os;
  struct b *WEAVEC_OWNED first = g;
  long v = first[i].n; // TRAP: pointer-out-of-bounds
  free(first);
  return v;
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  long v = 0;
  if (argc < 2)
    return 1;
  if (strcmp(argv[1], "grown") == 0)
    v = grown(7);
  else if (strcmp(argv[1], "ints") == 0)
    v = ints(5);
  else if (strcmp(argv[1], "member") == 0)
    v = member(1);
  printf("%ld\n", v);
  return 0;
}
