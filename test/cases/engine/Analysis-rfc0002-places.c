// Engine pin converted from test/Analysis/rfc0002-places.c; markers are the v0.10.0 golden diagnostics.
// RFC 0002: places are paths (`c.buf`, `p->in->buf`, `*pp`, `arr[*]`), not
// just variables.
#include "Inputs/prelude.h"

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
  use(c.buf); // BUG: use-after-free definite
}

void nested_arrows(struct outer *p) {
  free(p->in->buf);
  use(p->in->buf); // BUG: use-after-free definite
}

void deref_parameter(char **pp) {
  free(*pp);
  use(*pp); // BUG: use-after-free definite
}

int deref_freed_object(struct outer *c) {
  free(c);
  return c->n; // BUG: use-after-free definite
}

// RFC 0015: selected elements retain separate temporal and resource state.
void array_summary(void) {
  int *arr[4];
  arr[0] = malloc(4);
  arr[1] = malloc(4);
  free(arr[0]);
  use(arr[1]); free(arr[1]); // another element: fine
  use(arr[0]); // BUG: use-after-free definite
}

// Pointer arithmetic keeps the identity of the object (RFC 0004, *Pointer
// identity*): `q` is `p`.
void arithmetic(void) {
  char *p = malloc(4);
  char *q = p + 1;
  free(p);
  use(q); // BUG: use-after-free definite
}
