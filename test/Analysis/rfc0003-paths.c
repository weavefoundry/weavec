// RFC 0003: effects on paths reachable from parameters (`b->data`, `*out`)
// are part of the summary. The caller's memory is described by the callee's
// exit state, so a destroy-and-null helper leaves the field usable.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

struct buf {
  char *data;
  size_t len;
};

static void buf_init(struct buf *b, size_t n) {
  b->data = malloc(n);
  b->len = n;
}
static void buf_destroy(struct buf *b) { free(b->data); }
static void buf_reset(struct buf *b) {
  free(b->data);
  b->data = NULL;
}

void field_by_pointer(struct buf *b) {
  buf_destroy(b);
  // CHECK: rfc0003-paths.c:[[@LINE+1]]:3: error: use of 'b->data' after it was freed [weavec::use-after-free]
  b->data[0] = 1;
}

void field_by_address(void) {
  struct buf b;
  buf_init(&b, 8);
  buf_destroy(&b);
  // CHECK: rfc0003-paths.c:[[@LINE+1]]:3: error: use of 'b.data' after it was freed [weavec::use-after-free]
  b.data[0] = 1;
}

void destroy_idiom_is_fine(void) {
  struct buf b;
  buf_init(&b, 8);
  buf_reset(&b);
  buf_init(&b, 4);
  buf_reset(&b);
}

static int make(char **out) {
  *out = malloc(8);
  return *out != NULL;
}

void out_param(void) {
  char *s;
  if (!make(&s))
    return;
  free(s);
  // CHECK: rfc0003-paths.c:[[@LINE+1]]:3: error: use of 's' after it was freed [weavec::use-after-free]
  s[0] = 1;
}

void out_param_fine(void) {
  char *s;
  if (!make(&s))
    return;
  s[0] = 1;
  free(s);
}

// Re-pointing the parameter inside the callee does not hide what it freed.
static void free_and_null(char *p) {
  free(p);
  p = NULL;
}

void reassigned_parameter(void) {
  char *p = malloc(8);
  free_and_null(p);
  // CHECK: rfc0003-paths.c:[[@LINE+1]]:3: error: use of 'p' after it was freed [weavec::use-after-free]
  p[0] = 1;
}

// CHECK: 4 errors generated.
