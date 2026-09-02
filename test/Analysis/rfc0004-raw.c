// RFC 0004, "Raw pointers": a pointer cast from an integer, declared
// WEAVEC_RAW, loaded through a raw pointer or handed out as raw by a callee
// carries no ownership guarantee. Dereferencing, releasing, passing it where
// the callee does either, or asserting a kind for it is an unsafe-operation
// outside an unsafe region. Copying and comparing it is fine.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

typedef unsigned long uintptr_t;
struct node {
  int v;
  struct node *next;
};

void deref_int_cast(uintptr_t x) {
  struct node *n = (struct node *)x;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  n->v = 1;
  // CHECK-NEXT: {{.*}}n->v = 1;
  // CHECK-NEXT: {{.*}}^
  // CHECK-NEXT: rfc0004-raw.c:[[@LINE-5]]:20: note: 'n' is raw: cast from an integer here
  // CHECK: note: move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first
}

void deref_without_place(uintptr_t x) {
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer outside an unsafe region [weavec::unsafe-operation]
  ((struct node *)x)->v = 1;
  // CHECK: note: the pointer is raw: cast from an integer here
}

void copies_and_comparisons_are_fine(uintptr_t x, struct node *m) {
  struct node *n = (struct node *)x;
  struct node *o = n;
  if (o == m || n == NULL)
    return;
  n = m;
  n->v = 1; /* reassigned from a tracked pointer: no longer raw */
}

void release(uintptr_t x) {
  char *p = (char *)x;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:8: error: 'free' releases raw pointer 'p' outside an unsafe region [weavec::unsafe-operation]
  free(p);
}

void pass_to_borrowing_callee(uintptr_t x) {
  char *p = (char *)x;
  char *q = p;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:7: error: 'use' dereferences raw pointer 'q' outside an unsafe region [weavec::unsafe-operation]
  use(q);
  // CHECK: note: 'q' is raw: cast from an integer here (through 'p')
}

static void take(struct node *WEAVEC_OWNED n) { free(n); }
void pass_to_owning_callee(uintptr_t x) {
  struct node *n = (struct node *)x;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:8: error: 'take' takes ownership of raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  take(n);
}

// WEAVEC_RAW on a parameter, a field and a local.
void raw_param(struct node *WEAVEC_RAW r) {
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
  r->v = 1;
  // CHECK: rfc0004-raw.c:[[@LINE-3]]:40: note: 'r' is raw: declared WEAVEC_RAW here
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

// A callee's raw result is raw for the caller, through the inferred summary.
static void *lookup(uintptr_t x) { return (void *)x; }
void from_callee(uintptr_t x) {
  int *p = lookup(x);
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:4: error: dereference of raw pointer 'p' outside an unsafe region [weavec::unsafe-operation]
  *p = 1;
  // CHECK: note: 'p' is raw: handed out by 'lookup' here
}

// Rawness joins as "may be raw".
void joined(int c, uintptr_t x) {
  struct node *n = c ? malloc(sizeof *n) : (struct node *)x;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:3: error: dereference of raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  n->v = 1;
  // CHECK: rfc0004-raw.c:[[@LINE+1]]:8: error: 'free' releases raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  free(n);
}

// Pointer-to-integer conversion takes a value out of the model silently.
uintptr_t to_integer(struct node *WEAVEC_OWNED n) {
  return (uintptr_t)n;
}

// CHECK: 12 errors generated.
