// RFC 0017: numeric results, output values and access intervals compose.
// RUN: not %weavec --no-runtime --whole-program %s %S/Inputs/rfc0017-numeric.c -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --no-runtime --whole-program --ledger=%t.json %s %S/Inputs/rfc0017-numeric.c -- 2>/dev/null
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
//
// RFC 0031 §6.1: format-30 summaries carry no extent requirements and no
// numeric output expressions; a call's constant arguments reach the callee
// through the context it asks of the other unit (RFC 0031 §7 *Amendment
// (cross-unit contexts)*), and what that context stores on every return
// outside the caller's object is an error at the call (*Implementation
// amendments*, "Stores past the caller's object").
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
  // CHECK: rfc0017-numeric.c:[[@LINE+1]]:26: error: use of 'p' after it was freed [weavec::use-after-free]
  if (narrow(256) == 0) *p = 1;
}
void output(void) {
  unsigned char k; narrow_out(256, &k);
  int *p = malloc(sizeof *p); if (!p) return;
  free(p);
  // CHECK: rfc0017-numeric.c:[[@LINE+1]]:16: error: use of 'p' after it was freed [weavec::use-after-free]
  if (k == 0) *p = 1;
}
void before(void) {
  char a[4];
  // CHECK: rfc0017-numeric.c:[[@LINE+1]]:10: error: 'put_at' requires 'a' before its start [weavec::out-of-bounds]
  put_at(a, -1);
}
void minimum(void) {
  char a[4];
  // CHECK: rfc0017-numeric.c:[[@LINE+1]]:12: error: 'fill_min' requires 5 bytes behind 'a', which has 4 bytes [weavec::out-of-bounds]
  fill_min(a, 7, 5);
}
void product(void) {
  char *p = make_product(2, 3); if (!p) return;
  // `rows * cols` is no format-30 extent term; the context `make_product(2,
  // 3)` asks for has the extent 6.
  // CHECK: rfc0017-numeric.c:[[@LINE+1]]:3: error: 'p[6]' is out of bounds: index 6 of an object of 6 bytes [weavec::out-of-bounds]
  p[6] = 1; free(p);
}
void good(void) {
  char a[4]; put_at(a + 1, -1); fill_min(a, 8, 4);
  size_t size;
  if (checked_size(2, 3, &size)) return;
  char *p = malloc(size); if (!p) return;
  p[5] = 1; free(p);
  int *q = malloc(sizeof *q); if (!q) return;
  // `narrow(256)` is 0 and `calloc((size_t)-1, 2)` is null in the calls'
  // contexts: neither branch is taken.
  free(q); if (narrow(256) != 0) *q = 1;
  q = checked_allocation((size_t)-1, 2);
  if (q) { free(q); *q = 1; }
  unsigned x;
  reverse_outputs(&x, &x);
  // `x` is 2 (the callee writes `*b` before `*a`): this branch is never
  // taken.
  q = malloc(sizeof *q); if (!q) return;
  free(q); if (x == 1) *q = 1;
}
void ordered_outputs(void) {
  unsigned x;
  reverse_outputs(&x, &x);
  int *p = malloc(sizeof *p); if (!p) return;
  free(p);
  // CHECK: rfc0017-numeric.c:[[@LINE+1]]:16: error: use of 'p' after it was freed [weavec::use-after-free]
  if (x == 2) *p = 1;
}
// CHECK-NOT: {{warning|error}}:
// CHECK: 6 errors generated.
// The callees' accesses at a negative index and past the array are not
// proven (RFC 0017 §5: `counted(i + 1)` does not cover a signed index).
// LEDGER: "source": "{{.*}}Inputs/rfc0017-numeric.c",
// LEDGER: "text": "p[i]",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "unresolved",
// LEDGER: "text": "p[i]",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "unresolved",
