// RFC 0008, *Invalid releases*: a releaser is handed a pointer to a stack or
// static object, to a string literal, or into the middle of an allocation.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The RFC's snippets that must be reported.

static char table[16];

void stack(void) {
  char buf[8];
  char *p = buf;
  // CHECK: rfc0008-release.c:[[@LINE+2]]:3: error: 'p' is released but points to 'buf', which is not a heap object [weavec::invalid-release]
  // CHECK: rfc0008-release.c:[[@LINE-3]]:8: note: 'buf' is declared here
  free(p);
}

void object(void) {
  int x;
  int *p = &x;
  // CHECK: rfc0008-release.c:[[@LINE+2]]:3: error: 'p' is released but points to 'x', which is not a heap object [weavec::invalid-release]
  // CHECK: rfc0008-release.c:[[@LINE-3]]:7: note: 'x' is declared here
  free(p);
}

void global(void) {
  // CHECK: rfc0008-release.c:[[@LINE+2]]:3: error: 'table' is released but is not a heap object [weavec::invalid-release]
  // CHECK: rfc0008-release.c:[[@LINE-20]]:13: note: 'table' is declared here
  free(table);
}

void literal(void) {
  char *s = "hello";
  // CHECK: rfc0008-release.c:[[@LINE+1]]:3: error: 's' is released but points to a string literal [weavec::invalid-release]
  free(s);
}

void interior(void) {
  char *p = malloc(8);
  if (!p)
    return;
  char *q = p + 1;
  // CHECK: rfc0008-release.c:[[@LINE+2]]:3: error: 'q' is released but points 1 element past the start of its allocation [weavec::invalid-release]
  // CHECK: rfc0008-release.c:[[@LINE-5]]:13: note: allocated here
  free(q);
}

void searched(const char *s) {
  char *p = strdup(s);
  if (!p)
    return;
  char *q = strchr(p, 'x');
  if (!q) {
    free(p);
    return;
  }
  // CHECK: rfc0008-release.c:[[@LINE+2]]:3: error: 'q' is released but does not point to the start of its allocation [weavec::invalid-release]
  // CHECK: rfc0008-release.c:[[@LINE-9]]:13: note: allocated here
  free(q);
}

void arithmetic(void) {
  char *p = malloc(8);
  if (!p)
    return;
  // CHECK: rfc0008-release.c:[[@LINE+2]]:3: error: 'p' is released but points 1 element past the start of its allocation [weavec::invalid-release]
  // CHECK: rfc0008-release.c:[[@LINE-4]]:13: note: allocated here
  free(p + 1);
}

// Clean: heap objects released at their start, `p + 0`, and a pointer that
// walks forward and back again is the checker's business elsewhere.
void fine(void) {
  char *p = malloc(8);
  if (!p)
    return;
  char *q = p;
  free(q + 0);
  char *s = strdup("x");
  free(s);
}

// Clang itself warns about `free(table)`; WeaveC reports it too, once.
// CHECK: 1 warning and 7 errors generated.
