// RFC 0017: numeric results, output values and access intervals compose.
// RUN: not %weavec --strict-externs --whole-program %s %S/Inputs/rfc0017-numeric.c -- -ferror-limit=0 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
unsigned char narrow(unsigned);
void narrow_out(unsigned, unsigned char *);
void put_at(char *, int);
void fill_min(char *, size_t, size_t);
char *make_product(size_t, size_t);
int checked_size(size_t, size_t, size_t *);
void reverse_outputs(unsigned *, unsigned *);
void *checked_allocation(size_t, size_t);
void narrowed(void) {
  int *p = malloc(sizeof *p); if (!p) return;
  free(p);
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  if (narrow(256) == 0) *p = 1;
}
void output(void) {
  unsigned char k; narrow_out(256, &k);
  int *p = malloc(sizeof *p); if (!p) return;
  free(p);
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  if (k == 0) *p = 1;
}
void before(void) {
  char a[4];
  // CHECK: error: 'put_at' requires 'a' before its start [weavec::out-of-bounds]
  put_at(a, -1);
}
void minimum(void) {
  char a[4];
  // CHECK: error: 'fill_min' requires 5 bytes behind 'a', which has 4 bytes [weavec::out-of-bounds]
  fill_min(a, 7, 5);
}
void product(void) {
  char *p = make_product(2, 3); if (!p) return;
  // CHECK: error: 'p[6]' is out of bounds: index 6 of an object of 6 bytes [weavec::out-of-bounds]
  p[6] = 1; free(p);
}
void good(void) {
  char a[4]; put_at(a + 1, -1); fill_min(a, 8, 4);
  size_t size;
  if (checked_size(2, 3, &size)) return;
  char *p = malloc(size); if (!p) return;
  p[5] = 1; free(p);
  int *q = malloc(sizeof *q); if (!q) return;
  free(q); if (narrow(256) != 0) *q = 1;
  q = checked_allocation((size_t)-1, 2);
  if (q) { free(q); *q = 1; }
  unsigned x;
  reverse_outputs(&x, &x);
  q = malloc(sizeof *q); if (!q) return;
  free(q); if (x == 1) *q = 1;
}
void ordered_outputs(void) {
  unsigned x;
  reverse_outputs(&x, &x);
  int *p = malloc(sizeof *p); if (!p) return;
  free(p);
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  if (x == 2) *p = 1;
}
// CHECK: 6 errors generated.
