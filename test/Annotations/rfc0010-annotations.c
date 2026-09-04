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
// through it, so the share the local takes and drops is a leak.
struct node {
  int WEAVEC_REFCOUNT refs;
  struct node *next;
};
// DUMP-LABEL: function 'retain_local':
// DUMP: summary: n->next: read; n->next->refs: read|written; stores{} returns{} requires{n} increments{n->next->refs}
void retain_local(struct node *n) {
  struct node *p = n->next;
  // CHECK: rfc0010-annotations.c:[[@LINE+1]]:3: warning: 'p' is leaked [weavec::leak]
  p->refs++;
  // CHECK: rfc0010-annotations.c:[[@LINE-1]]:3: note: reference taken here
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

// CHECK: 3 warnings and 3 errors generated.
