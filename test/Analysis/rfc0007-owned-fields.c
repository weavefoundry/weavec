// RFC 0007, *Owned fields*: a field declared `WEAVEC_OWNED`, or one this
// function stored an owned value into, is lost when its container is freed
// without it; a caller's alias of a field the callee frees with the object
// is a use after free (the deepest-first ordering fix).
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

char *strdup(const char *s);

struct box {
  char *WEAVEC_OWNED p;
};

struct buf {
  char *data;
  size_t n;
};

// The RFC's snippets that must be reported.

void declared(struct box *b) {
  // CHECK: rfc0007-owned-fields.c:[[@LINE+2]]:3: warning: 'b->p' is leaked when 'b' is freed [weavec::leak]
  // CHECK: rfc0007-owned-fields.c:{{[0-9]+}}:22: note: 'p' is declared WEAVEC_OWNED here
  free(b);
}

void inferred(struct buf *b) {
  b->data = malloc(8);
  // CHECK: rfc0007-owned-fields.c:[[@LINE+2]]:3: warning: 'b->data' is leaked when 'b' is freed [weavec::leak]
  // CHECK: rfc0007-owned-fields.c:[[@LINE-2]]:13: note: allocated here
  free(b);
}

void elements(char **a, int n) {
  a[0] = strdup("x");
  // CHECK: rfc0007-owned-fields.c:[[@LINE+1]]:3: warning: '*a' is leaked when 'a' is freed [weavec::leak]
  free(a);
}

// Newly caught after the ordering fix: the caller's alias of a field the
// callee frees together with the object.
static void both(struct box *b) {
  free(b->p);
  free(b);
}

void alias_of_field(struct box *b) {
  char *q = b->p;
  both(b);
  // CHECK: rfc0007-owned-fields.c:[[@LINE+2]]:3: error: use of 'q' after it was freed [weavec::use-after-free]
  // CHECK: rfc0007-owned-fields.c:[[@LINE-2]]:3: note: freed here (through 'b->p')
  q[0] = 1;
}

static void both_reversed(struct box *b) {
  char *t = b->p;
  free(b);
  free(t);
}

void alias_of_field_reversed(struct box *b) {
  char *q = b->p;
  both_reversed(b);
  // CHECK: rfc0007-owned-fields.c:[[@LINE+1]]:3: error: use of 'q' after it was freed [weavec::use-after-free]
  q[0] = 1;
}

// Clean (RFC 0007, *Deliberately not caught* and *Diagnostics*).

// Fields of a fresh object own nothing until this function stores into them.
void fresh_error_path(void) {
  struct box *b = malloc(sizeof *b);
  if (!b)
    return;
  free(b);
}

static struct box *box_new(void) { return malloc(sizeof(struct box)); }
void fresh_from_callee(void) {
  struct box *b = box_new();
  if (!b)
    return;
  free(b);
}

void nulled(struct box *b) {
  free(b->p);
  b->p = NULL;
  free(b);
}

void maybe(struct box *b) {
  if (b->p)
    free(b->p);
  free(b);
}

void moved_out(struct box *b, char **out) {
  *out = b->p;
  free(b);
}

// A constructor stores into a field it knows nothing about: no report.
void init(struct box *b, char *WEAVEC_OWNED p) {
  b->p = p;
}

// CHECK: 3 warnings and 2 errors generated.
