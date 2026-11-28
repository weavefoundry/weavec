// RFC 0055 §3 (D3): in a fast unit a `realloc` gives a block of the old
// block's type, from that type's pool, whatever its site infers: a block
// of `struct a` that grows into another class is still one, so converting
// it to `struct b` stops (`bad-cast`), where a block of no type would have
// taken the type of its first conversion. Converted back to its own type
// it is used and released as one.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s --check-prefix=KEPT
// RUN: not --crash %t cast 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct a {
  char *WEAVEC_OWNED name;
};
struct b {
  long n;
};

// CHECK: weavec: bad-cast at {{.*}}realloc-typed.c:[[@LINE+5]]:{{[0-9]+}}: {{.*}}'struct b'
// CHECK: {{.*}}'struct a'
long viewed(void *WEAVEC_OWNED moved, int cast) {
  long n = 0;
  if (cast) {
    struct b *bs = moved;
    n = bs->n;
  }
  struct a *WEAVEC_OWNED back = moved;
  n += back->name == NULL;
  free(back->name);
  free(back);
  return n;
}

long grown(int cast) {
  struct a *WEAVEC_OWNED as = calloc(1, sizeof *as);
  if (as == NULL)
    return -1;
  void *WEAVEC_OWNED moved = realloc(as, 64 * sizeof *as);
  if (moved == NULL) {
    free(as);
    return -1;
  }
  return viewed(moved, cast);
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  // KEPT: kept 1
  printf("kept %ld\n", grown(argc > 1 && strcmp(argv[1], "cast") == 0));
  return 0;
}
