// RFC 0010, *Annotations*: WEAVEC_RETAINS and WEAVEC_RELEASES describe a
// library's ref/unref pair with no body in view, WEAVEC_REFCOUNT marks a
// field as a count so a lost share is reported, and WEAVEC_OWNED_BY names
// the release family of a WEAVEC_OWNED result or parameter.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

#if WEAVEC_H_VERSION_MINOR < 5
#error "weavec.h 0.5 spells the RFC 0010 annotations"
#endif

struct gobj;
struct gobj *g_new(void) WEAVEC_OWNED;
struct gobj *g_ref(struct gobj *WEAVEC_RETAINS o);
void g_unref(struct gobj *WEAVEC_RELEASES o);

// Clean: the returning ref's result is a copy of its argument (the shape of
// `g_object_ref`), so `b` carries the share `g_ref` took.
int balanced(void) {
  struct gobj *a = g_new();
  if (!a)
    return -1;
  struct gobj *b = g_ref(a);
  g_unref(b);
  g_unref(a);
  return 0;
}

int twice(void) {
  struct gobj *a = g_new();
  if (!a)
    return -1;
  g_unref(a);
  // CHECK: rfc0010-annotations.c:[[@LINE+1]]:3: error: 'a' is released twice [weavec::double-free]
  g_unref(a);
  return 0;
}

int after(void) {
  struct gobj *a = g_new();
  if (!a)
    return -1;
  g_unref(a);
  // CHECK: rfc0010-annotations.c:[[@LINE+1]]:7: error: use of 'a' after its reference was released [weavec::use-after-free]
  use(a);
  return 0;
}

// WEAVEC_REFCOUNT: the field is a count even though nothing here releases
// through it, so the share the local takes and drops is a leak. The object
// engine does not read WEAVEC_REFCOUNT: the increment is an integer store
// that leaves the caller's count unknown, no share is taken and no leak is
// reported (the retired golden comparison of RFC 0030, *Lit tests*). A leak is never
// a facet (RFC 0030 §3.4), so no outcome is lost.
struct node {
  int WEAVEC_REFCOUNT refs;
  struct node *next;
};
// DUMP-LABEL: function 'retain_local':
// DUMP-NEXT: summary:
// DUMP-NEXT: always-returns
// DUMP-NEXT: store param0->next->refs := int [-2147483648, 2147483647]
void retain_local(struct node *n) {
  struct node *p = n->next;
  p->refs++;
}
struct sized {
  int len;
};
void not_a_count(struct sized *s) {
  struct sized *t = s;
  t->len++;
}

// WEAVEC_OWNED_BY: the family of an owned result and an owned parameter.
struct handle;
void handle_close(struct handle *WEAVEC_OWNED h);
struct handle *handle_open(void) WEAVEC_OWNED WEAVEC_OWNED_BY(handle_close);
void handle_take(struct handle *WEAVEC_OWNED WEAVEC_OWNED_BY(handle_close) h);

int right(void) {
  struct handle *h = handle_open();
  if (!h)
    return -1;
  handle_close(h);
  return 0;
}
int wrong(void) {
  struct handle *h = handle_open();
  if (!h)
    return -1;
  // CHECK: rfc0010-annotations.c:[[@LINE+1]]:3: error: 'h' is released with 'free' but must be released with 'handle_close' [weavec::mismatched-release]
  free(h);
  return 0;
}

// Contradictions on a definition (RFC 0010, *Diagnostics*).
// CHECK: rfc0010-annotations.c:[[@LINE+1]]:55: warning: 'o' is declared both WEAVEC_RETAINS and WEAVEC_RELEASES [weavec::invalid-annotation]
void both(struct gobj *WEAVEC_RETAINS WEAVEC_RELEASES o) { use(o); }
// CHECK: rfc0010-annotations.c:[[@LINE+1]]:16: warning: 'family_alone' is declared WEAVEC_OWNED_BY(handle_close) without WEAVEC_OWNED [weavec::invalid-annotation]
struct handle *family_alone(void) WEAVEC_OWNED_BY(handle_close) { return NULL; }

// CHECK: 2 warnings and 3 errors generated.
