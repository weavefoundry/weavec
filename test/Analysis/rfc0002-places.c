// RFC 0002: places are paths (`c.buf`, `p->in->buf`, `*pp`, `arr[*]`), not
// just variables.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

struct inner {
  int *buf;
};
struct outer {
  struct inner *in;
  int n;
};

void member_of_local(void) {
  struct inner c;
  c.buf = malloc(4);
  free(c.buf);
  // CHECK: rfc0002-places.c:[[@LINE+1]]:7: error: use of 'c.buf' after it was freed [weavec::use-after-free]
  use(c.buf);
}

void nested_arrows(struct outer *p) {
  free(p->in->buf);
  // CHECK: rfc0002-places.c:[[@LINE+1]]:7: error: use of 'p->in->buf' after it was freed [weavec::use-after-free]
  use(p->in->buf);
}

void deref_parameter(char **pp) {
  free(*pp);
  // CHECK: rfc0002-places.c:[[@LINE+1]]:7: error: use of '*pp' after it was freed [weavec::use-after-free]
  use(*pp);
}

int deref_freed_object(struct outer *c) {
  free(c);
  // CHECK: rfc0002-places.c:[[@LINE+1]]:10: error: use of 'c' after it was freed [weavec::use-after-free]
  return c->n;
}

// All elements of an array share one summary place.
void array_summary(void) {
  int *arr[4];
  arr[0] = malloc(4);
  arr[1] = malloc(4);
  free(arr[0]);
  // CHECK: rfc0002-places.c:[[@LINE+1]]:7: error: use of 'arr[*]' after it was freed [weavec::use-after-free]
  use(arr[1]);
}

// Pointer arithmetic keeps the identity of the object (RFC 0004, *Pointer
// identity*): `q` is `p`.
void arithmetic(void) {
  char *p = malloc(4);
  char *q = p + 1;
  free(p);
  // CHECK: rfc0002-places.c:[[@LINE+1]]:7: error: use of 'q' after it was freed [weavec::use-after-free]
  use(q);
  // CHECK: rfc0002-places.c:[[@LINE-3]]:3: note: freed here (through 'p')
}

// CHECK: 6 errors generated.
