// RFC 0013: input identities survive copied fields and local alias swaps.
// RUN: not %weavec --whole-program %s %S/Inputs/heap13.c -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/heap13.h"

void good_copy(void) {
  struct heap13_box a = {malloc(4)}, b = {malloc(8)};
  char *old = b.data; heap13_copy_and_free(&a, &b); free(old);
}
void bad_copy(void) {
  struct heap13_box a = {malloc(4)}, b = {malloc(8)};
  char *old = b.data; heap13_copy_and_free(&a, &b); free(old);
  // CHECK: rfc0013-input-identity.c:[[@LINE+1]]:3: error: 'b.data' is freed twice [weavec::double-free]
  free(b.data);
}
void good_swap(void) {
  struct heap13_box a = {malloc(4)}, b = {malloc(8)};
  heap13_local_swap(&a, &b);
  if (a.data) a.data[7] = 0;
  if (b.data) b.data[3] = 0;
  free(a.data); free(b.data);
}
void bad_swap(void) {
  struct heap13_box a = {malloc(4)}, b = {malloc(8)};
  heap13_local_swap(&a, &b);
  // CHECK: rfc0013-input-identity.c:[[@LINE+1]]:15: error: 'b.data[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  if (b.data) b.data[4] = 0;
  free(a.data); free(b.data);
}
int main(void) { return 0; }
// CHECK: 2 errors generated.
