// RFC 0038 amendment 15: under a limit on the address space too small for
// the shadow and the arena (`ulimit -v`), a program runs unchecked, as the
// warning says, and allocates from the C library: its blocks are
// zero-filled, aligned, resized and released as the arena's are.
// REQUIRES: host-linux
// RUN: %weavec_cc -O2 %s -o %t
// RUN: sh -c 'ulimit -v 100000 && %t' 2>&1 | FileCheck %s
// CHECK: weavec: warning: the shadow memory could not be reserved
// CHECK: ok
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  unsigned char *p = calloc(1000, 1);
  void *aligned = aligned_alloc(256, 512);
  unsigned i;
  int zero = 1;
  if (p == NULL || aligned == NULL || ((uintptr_t)aligned & 255) != 0)
    return 1;
  for (i = 0; i < 1000; ++i)
    zero &= p[i] == 0;
  memset(p, 7, 1000);
  p = realloc(p, 100000);
  if (!zero || p == NULL || p[999] != 7)
    return 2;
  free(p);
  free(aligned);
  puts("ok");
  return 0;
}
