// RFC 0008, *Uninitialised pointers*: a pointer variable, or a pointer field
// of a record variable, declared without an initialiser holds nothing until
// it is assigned; reading, dereferencing, copying or releasing it before then
// is an error. Initialising it by any route (assignment, a callee's store, a
// mutable borrow, `memset`) clears the fact.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdlib.h>
#include <string.h>

struct buf {
  char *data;
  int len;
};

// The RFC's snippets that must be reported.

void local(void) {
  char *p;
  // CHECK: rfc0008-uninit.c:[[@LINE+2]]:3: error: use of 'p' before it was initialized [weavec::use-of-uninitialized]
  // CHECK: rfc0008-uninit.c:[[@LINE-2]]:9: note: 'p' is declared here
  free(p);
}

int field(void) {
  struct buf b;
  b.len = 0;
  // CHECK: rfc0008-uninit.c:[[@LINE+2]]:10: error: use of 'b.data' before it was initialized [weavec::use-of-uninitialized]
  // CHECK: rfc0008-uninit.c:[[@LINE-3]]:14: note: 'b' is declared here
  return b.data[0];
}

void copied(void) {
  char *p;
  // CHECK: rfc0008-uninit.c:[[@LINE+1]]:13: error: use of 'p' before it was initialized [weavec::use-of-uninitialized]
  char *q = p;
  free(q);
}

int maybe(int c) {
  char *p;
  if (c)
    p = malloc(4);
  // CHECK: rfc0008-uninit.c:[[@LINE+1]]:3: error: use of 'p' before it was initialized [weavec::use-of-uninitialized]
  free(p);
  return 0;
}

// Clean: every way of initialising the place.
static void init(char **out) { *out = malloc(4); }

int clean(int c) {
  char *a;
  a = malloc(4);
  free(a);
  char *b;
  init(&b);
  free(b);
  struct buf s;
  memset(&s, 0, sizeof s);
  free(s.data);
  struct buf t = {0};
  free(t.data);
  static char *st;
  free(st);
  char *late;
  if (c)
    late = malloc(2);
  else
    late = NULL;
  free(late);
  return 0;
}

// The record never reaches a summary: only locals can be uninitialised.
// DUMP: function 'local':
// DUMP: summary: stores{} returns{}
// DUMP: function 'maybe':
// DUMP: summary: stores{} returns{}

// CHECK: 4 errors generated.
