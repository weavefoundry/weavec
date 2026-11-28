// RFC 0054 §2.2, amendment 22: a safe C function's pointer converted from a
// `void *` is bounded by the objects of its type the runtime finds from it,
// the rest of an array of the type. A block `calloc` made for 6 elements and
// `realloc` grew to 7 through a `void *` is read at element 6 in the fast,
// the checked and the unoptimized build; a read at element 7, past the new
// size, stops with `pointer-out-of-bounds`.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t.fast
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: %weavec_cc -O0 -fweavec-safe %s -o %t.O0
// RUN: %t.fast 6 | FileCheck %s --check-prefix=OK
// RUN: %t.full 6 | FileCheck %s --check-prefix=OK
// RUN: %t.O0 6 | FileCheck %s --check-prefix=OK
// RUN: not --crash %t.fast 7 2>&1 | FileCheck %s --check-prefix=PAST
// RUN: not --crash %t.full 7 2>&1 | FileCheck %s --check-prefix=PAST
// RUN: not --crash %t.O0 7 2>&1 | FileCheck %s --check-prefix=PAST
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct b { long n; long m; };

__attribute__((noinline)) long grow(int i) {
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
  long v = ds[i].n;
  free(ds);
  return v;
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  if (argc < 2)
    return 1;
  printf("grow %ld\n", grow(atoi(argv[1])));
  return 0;
}
// OK: grow 42
// PAST: weavec: pointer-out-of-bounds at {{.*}}cast-extent.c:[[@LINE-12]]:18: read of 8 bytes at 0x{{[0-9a-f]+}}
// PAST: weavec: 0x{{[0-9a-f]+}} is 0 bytes after the 112-byte bounds of the pointer at 0x{{[0-9a-f]+}}
