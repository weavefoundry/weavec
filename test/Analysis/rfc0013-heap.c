// RFC 0013: reachable child ownership, aliases, sizes and strings survive calls.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include "../evaluation/Inputs/heap.c"

// DUMP-LABEL: function 'string_box':
// DUMP: heap result complete{result->data = fresh(free) extent=4 length=3}

void overflow(void) {
  struct box *b = box_new(); if (!b) return;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: 'b->data[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  b->data[4] = 0;
  free(b->data); free(b);
}
void leak(void) {
  struct box *b = box_new(); if (!b) return;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: warning: 'b->data' is leaked when 'b' is freed [weavec::leak]
  free(b);
}
void alias(void) {
  char *p = malloc(4); if (!p) return;
  struct box *b = box_wrap(p);
  if (!b) { free(p); return; }
  free(p);
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: use of 'b->data' after it was freed [weavec::use-after-free]
  b->data[0] = 0;
  free(b);
}
void changed_size(void) {
  size_t n = 4;
  char *p = malloc(n); if (!p) return;
  n = 8;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: 'p[7]' is out of bounds: index 7 of an object of 4 bytes [weavec::out-of-bounds]
  p[7] = 0;
  free(p);
}
struct box *empty(void) {
  struct box *b = box_new(); if (!b) return NULL;
  free(b->data); b->data = NULL; return b;
}
void null_field(void) {
  struct box *b = empty(); if (!b) return;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: dereference of 'b->data', which is null [weavec::null-dereference]
  b->data[0] = 0;
  free(b);
}
char *strcpy(char *, const char *);
struct box *string_box(void) {
  struct box *b = box_new(); if (!b) return NULL;
  strcpy(b->data, "abc"); return b;
}
void string_field(void) {
  struct box *b = string_box(); if (!b) return;
  char dst[3];
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:10: error: 'strcpy' accesses 4 bytes of 'dst', which has 3 bytes [weavec::out-of-bounds]
  strcpy(dst, b->data);
  free(b->data); free(b);
}

// CHECK: 1 warning and 5 errors generated.
