// RFC 0002: every scope gets a lifetime; a borrow whose holder outlives the
// borrowed object is `lifetime-too-short`.
// RUN: not %weavec %s -- -Wno-return-stack-address 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

int *gp;
static int g;

int *return_local_via_pointer(void) {
  int x = 1;
  int *p = &x;
  // CHECK: rfc0002-lifetimes.c:[[@LINE+1]]:10: error: 'p' may outlive 'x', which it points to [weavec::lifetime-too-short]
  return p;
  // CHECK: rfc0002-lifetimes.c:[[@LINE-4]]:7: note: 'x' is declared here
}

int *return_address_of_local(void) {
  int x = 1;
  // CHECK: rfc0002-lifetimes.c:[[@LINE+1]]:10: error: returned pointer may outlive 'x', which it points to [weavec::lifetime-too-short]
  return &x;
}

void escapes_inner_scope(void) {
  int *p;
  {
    int x = 1;
    // CHECK: rfc0002-lifetimes.c:[[@LINE+1]]:5: error: 'p' may outlive 'x', which it points to [weavec::lifetime-too-short]
    p = &x;
    // CHECK: rfc0002-lifetimes.c:[[@LINE-3]]:9: note: 'x' is declared here
    // CHECK: rfc0002-lifetimes.c:[[@LINE+1]]:3: note: 'x' goes out of scope here
  }
  use(p);
}

void escapes_through_out_parameter(int **out) {
  int x = 1;
  // CHECK: rfc0002-lifetimes.c:[[@LINE+1]]:3: error: '*out' may outlive 'x', which it points to [weavec::lifetime-too-short]
  *out = &x;
}

void escapes_to_global(void) {
  int x = 1;
  // CHECK: rfc0002-lifetimes.c:[[@LINE+1]]:3: error: 'gp' may outlive 'x', which it points to [weavec::lifetime-too-short]
  gp = &x;
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

// CHECK: 5 errors generated.
