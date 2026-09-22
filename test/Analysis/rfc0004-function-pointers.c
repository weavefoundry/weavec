// RFC 0004, "Signatures for function pointers": an indirect call is resolved
// through ownership annotations on the function-pointer type, else through
// the join of every address-taken function of that type in the translation
// unit; with neither it is a call into unknown code like an unannotated
// extern: RFC 0030 §5.1 records it as an `unresolved(unknown-callee)` row.
// RUN: not %weavec --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include "../Inputs/prelude.h"
#include <weavec.h>

struct node {
  int v;
  struct node *next;
};

// 1. Annotations on the type: a typedef of a function-pointer type.
typedef void (*dtor_t)(void *WEAVEC_OWNED);
void through_typedef(dtor_t dtor, struct node *n) {
  dtor(n);
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:7: error: use of 'n' after it was moved [weavec::use-after-move]
  use(n);
}

// An ownership annotation on the declarator describes the result.
typedef WEAVEC_OWNED struct node *(*maker_t)(void);
void owned_result(maker_t make) {
  struct node *n = make();
  free(n);
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:7: error: use of 'n' after it was freed [weavec::use-after-free]
  use(n);
}

// A field of function-pointer type, annotated inline.
struct allocator {
  void *(*WEAVEC_OWNED alloc)(size_t);
  void (*release)(void *WEAVEC_OWNED);
};
void through_field(struct allocator *a) {
  struct node *n = a->alloc(sizeof *n);
  a->release(n);
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:7: error: use of 'n' after it was moved [weavec::use-after-move]
  use(n);
}

// A parameter of function-pointer type, annotated inline.
void through_param(void (*drop)(struct node *WEAVEC_OWNED), struct node *n) {
  drop(n);
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:7: error: use of 'n' after it was moved [weavec::use-after-move]
  use(n);
}

// 2. The join of address-taken functions of the type.
static void node_free(struct node *n) { free(n); }
static void node_peek(struct node *n) { use(n); }
struct hooks {
  void (*on_drop)(struct node *);
};
static struct hooks H = {node_free};
void through_table(struct node *n) {
  H.on_drop(n);
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:7: error: use of 'n' after it was freed [weavec::use-after-free]
  use(n);
}
void register_peek(struct hooks *h) { h->on_drop = node_peek; }
void through_callback(void (*cb)(struct node *), struct node *n) {
  // RFC 0030 §9.3: an indirect call whose slot has no known target takes
  // the §5.1 default with the reason `callback`.
  // LEDGER: "text": "cb(n)",
  // LEDGER: "reason": "callback",
  // LEDGER-NEXT: "detail": "the target of 'cb' is unknown; annotate the parameters of its function type",
  cb(n); /* RFC 0014: a type match alone does not identify this value. */
  use(n);
}

// A function's own address is not "taken" by calling it directly.
static int helper(int x) { return x; }
int calls_directly(int x) { return helper(x); }

// 3. Otherwise unknown code, at every call.
void boundary(int (*cmp)(const void *, const void *), char *a, char *b) {
  // LEDGER: "text": "cmp(a,b)",
  // LEDGER: "reason": "callback",
  cmp(a, b);
  // LEDGER: "text": "cmp(b,a)",
  // LEDGER: "reason": "callback",
  cmp(b, a);
}

// An unresolvable callee that is not a place.
static struct node *(*hook)(void);
static struct node *(*get_hook(void))(void) { return hook; }
void boundary_without_place(void) {
  // LEDGER: "text": "get_hook()()",
  // LEDGER: "reason": "callback",
  // LEDGER-NEXT: "detail": "the target of a function pointer is unknown; annotate the parameters of its function type",
  struct node *n = get_hook()();
  use(n);
}

// CHECK-NOT: warning:
// CHECK: 5 errors generated.
