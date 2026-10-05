// RFC 0004, "Raw pointers", as amended by RFC 0033 §2: a pointer declared
// WEAVEC_RAW, loaded through a raw pointer or handed out as raw by a callee
// carries no ownership guarantee. Dereferencing, releasing, passing it where
// the callee does either, or asserting a kind for it is an unsafe-operation
// outside an unsafe region. Copying and comparing it is fine. A pointer
// converted from an integer is not raw: it is a pointer of unknown
// provenance, guarded where it is used.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s --implicit-check-not='cast from an integer'
#include "../Inputs/prelude.h"
#include <weavec.h>

typedef unsigned long uintptr_t;
struct node {
  int v;
  struct node *next;
};

// RFC 0033 §2: no error for a pointer converted from an integer.
void deref_int_cast(uintptr_t x) {
  struct node *n = (struct node *)x;
  // CHECK-NOT: rfc0004-raw.c:[[@LINE+1]]:{{.*}}error
  n->v = 1;
  ((struct node *)x)->v = 1;
  free((char *)x);
}

void copies_and_comparisons_are_fine(struct node *WEAVEC_RAW x,
                                     struct node *m) {
  struct node *n = x;
  struct node *o = n;
  if (o == m || n == NULL)
    return;
  n = m;
  n->v = 1; /* reassigned from a tracked pointer: no longer raw */
}

void release(char *WEAVEC_RAW p) {
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:8: error: 'free' releases raw pointer 'p' outside an unsafe region [weavec::unsafe-operation]
  free(p);
}

void pass_to_borrowing_callee(char *WEAVEC_RAW p) {
  char *q = p;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:7: error: 'use' dereferences raw pointer 'q' outside an unsafe region [weavec::unsafe-operation]
  use(q);
  // CHECK: note: 'q' is raw: declared WEAVEC_RAW here (through 'p')
}

static void take(struct node *WEAVEC_OWNED n) { free(n); }
void pass_to_owning_callee(struct node *WEAVEC_RAW n) {
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:8: error: 'take' takes ownership of raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  take(n);
}

// WEAVEC_RAW on a parameter, a field and a local.
void raw_param(struct node *WEAVEC_RAW r) {
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
  r->v = 1;
  // CHECK-NEXT: {{.*}}r->v = 1;
  // CHECK-NEXT: {{.*}}^
  // CHECK-NEXT: rfc0004-raw.c:[[@LINE-5]]:40: note: 'r' is raw: declared WEAVEC_RAW here
  // CHECK: note: move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first
}

struct ctx {
  void *WEAVEC_RAW cookie;
};
void raw_field(struct ctx *c) {
  struct node *n = c->cookie;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:7: error: 'use' dereferences raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  use(n);
  // CHECK: note: 'n' is raw: declared WEAVEC_RAW here (through 'c->cookie')
}

void raw_local(struct node *m) {
  struct node *WEAVEC_RAW r = m; /* storing into a raw place drops the value out of the model */
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
  r->v = 1;
  m->v = 1; /* m itself is untouched */
}

// A value loaded through a raw pointer is raw.
void loaded_through_raw(struct node *WEAVEC_RAW r) {
  struct node *n;
  WEAVEC_UNSAFE { n = r->next; }
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  n->v = 1;
  // CHECK: note: 'n' is raw: loaded through raw pointer 'r' here
}

// A callee's raw result is raw for the caller, through the inferred summary:
// here the summary returns the raw argument itself.
static void *lookup(void *WEAVEC_RAW x) { return x; }
void from_callee(void *WEAVEC_RAW x) {
  int *p = lookup(x);
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:4: error: dereference of raw pointer 'p' outside an unsafe region [weavec::unsafe-operation]
  *p = 1;
  // CHECK: note: 'p' is raw: declared WEAVEC_RAW here (through 'x')
}

// RFC 0033 §2: rawness is a must fact. A value raw on some paths only is
// guarded, not an error.
void joined(int c, struct node *WEAVEC_RAW x) {
  struct node *n = c ? malloc(sizeof *n) : x;
  // CHECK-NOT: rfc0004-raw.c:[[@LINE+1]]:{{.*}}error
  n->v = 1;
  free(n);
}

// Pointer-to-integer conversion takes a value out of the model silently.
uintptr_t to_integer(struct node *WEAVEC_OWNED n) {
  return (uintptr_t)n;
}

// CHECK: 8 errors generated.
