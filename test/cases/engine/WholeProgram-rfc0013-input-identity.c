// Engine pin converted from test/WholeProgram/rfc0013-input-identity.c; markers are the v0.10.0 golden diagnostics.
// UNITS: Inputs/heap13.c
// RFC 0013: input identities survive copied fields and local alias swaps.
#include "Inputs/prelude.h"
#include "Inputs/heap13.h"

void good_copy(void) {
  struct heap13_box a = {malloc(4)}, b = {malloc(8)};
  char *old = b.data; heap13_copy_and_free(&a, &b); free(old);
}
void bad_copy(void) {
  struct heap13_box a = {malloc(4)}, b = {malloc(8)};
  char *old = b.data; heap13_copy_and_free(&a, &b); free(old);
  free(b.data); // BUG: double-free definite
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
  if (b.data) b.data[4] = 0; // BUG: out-of-bounds definite
  free(a.data); free(b.data);
}
int main(void) { return 0; }
