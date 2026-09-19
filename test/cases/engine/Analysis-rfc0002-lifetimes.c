// Engine pin converted from test/Analysis/rfc0002-lifetimes.c; markers are the v0.10.0 golden diagnostics.
// FLAGS: -Wno-return-stack-address
// RFC 0002: every scope gets a lifetime; a borrow whose holder outlives the
// borrowed object is `lifetime-too-short`.
#include "Inputs/prelude.h"

int *gp;
static int g;

int *return_local_via_pointer(void) {
  int x = 1;
  int *p = &x;
  return p; // BUG: lifetime-too-short definite
}

int *return_address_of_local(void) {
  int x = 1;
  return &x; // BUG: lifetime-too-short definite
}

void escapes_inner_scope(void) {
  int *p;
  {
    int x = 1;
    p = &x; // BUG: lifetime-too-short definite
  }
  use(p);
}

void escapes_through_out_parameter(int **out) {
  int x = 1;
  *out = &x; // BUG: lifetime-too-short definite
}

void escapes_to_global(void) {
  int x = 1;
  gp = &x; // BUG: lifetime-too-short definite
}

// Clean: the borrowed object outlives the holder.
void inner_holder(void) {
  int x = 1;
  {
    int *p = &x;
    use(p);
  }
}

void assigned_in_inner_scope(void) {
  int *p;
  int x = 0;
  {
    p = &x;
  }
  use(p);
}

void statics_live_forever(void) {
  static int s;
  gp = &s;
  int *p = &g;
  use(p);
}
