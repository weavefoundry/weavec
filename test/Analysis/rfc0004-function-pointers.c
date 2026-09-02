// RFC 0004, "Signatures for function pointers": an indirect call is resolved
// through ownership annotations on the function-pointer type, else through
// the join of every address-taken function of that type in the translation
// unit; with neither it is a checking boundary like an unannotated extern.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --strict-externs %s -- 2>&1 | FileCheck --check-prefix=STRICT %s
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
  cb(n); /* may be node_free or node_peek: the join frees */
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:7: error: use of 'n' after it was freed [weavec::use-after-free]
  use(n);
}

// A function's own address is not "taken" by calling it directly.
static int helper(int x) { return x; }
int calls_directly(int x) { return helper(x); }

// 3. Otherwise a boundary: once per function type by default.
void boundary(int (*cmp)(const void *, const void *), char *a, char *b) {
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:3: warning: call through 'cmp' is not checked: its function type has no ownership annotations and no function of that type has its address taken in this translation unit [weavec::annotation-required]
  cmp(a, b);
  // CHECK-NEXT: {{.*}}cmp(a, b);
  // CHECK-NEXT: {{.*}}^
  // CHECK-NEXT: rfc0004-function-pointers.c:[[@LINE-3]]:3: note: annotate the parameters of its function type with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or take the address of a function of that type in this translation unit
  cmp(b, a);
  // STRICT: rfc0004-function-pointers.c:[[@LINE-5]]:3: error: unchecked call through 'cmp' outside an unsafe region [weavec::unsafe-operation]
  // STRICT: rfc0004-function-pointers.c:[[@LINE-2]]:3: error: unchecked call through 'cmp' outside an unsafe region [weavec::unsafe-operation]
}

// An unresolvable callee that is not a place.
static struct node *(*hook)(void);
static struct node *(*get_hook(void))(void) { return hook; }
void boundary_without_place(void) {
  // CHECK: rfc0004-function-pointers.c:[[@LINE+1]]:20: warning: call through a function pointer is not checked
  struct node *n = get_hook()();
  use(n);
  // STRICT: rfc0004-function-pointers.c:[[@LINE-2]]:20: error: unchecked call through a function pointer outside an unsafe region [weavec::unsafe-operation]
}

// CHECK: 2 warnings and 6 errors generated.
