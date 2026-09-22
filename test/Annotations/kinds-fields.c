// RFC 0030 §7.4 rule 7 and §7.6: stores that update a pointer and its count
// (per a declared kind or a §7.6 candidate) form one group when they occur
// in one basic block with no intervening call, loop or access through the
// object; the obligation is decided after the group's last store. The §7.6
// candidates `count(d) == f + c` and `bytes(d) == f + c` (c in {0, 1}) of a
// struct's pointer and integer fields are proposed for Houdini, except for a
// struct some write the store-group rule cannot see reaches: a field's
// address taken, a byte-wise write, a conversion from another type.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s
#include <weavec.h>

typedef unsigned long size_t;
void *malloc(size_t);
void *memcpy(void *, const void *, size_t);

struct vec {
  int *data;
  size_t len;
};
// CHECK: candidate struct vec: count(data) == len + 0
// CHECK-NEXT: candidate struct vec: bytes(data) == len + 0
// CHECK-NEXT: candidate struct vec: count(data) == len + 1
// CHECK-NEXT: candidate struct vec: bytes(data) == len + 1

// A declared kind needs no candidate; its stores still group.
struct text {
  char *WEAVEC_COUNTED_BY(cap) buf;
  size_t cap;
};

// CHECK: disqualified struct taken: the address of 'taken.n' is taken
struct taken { int *p; int n; };
// CHECK: disqualified struct copied: an object of 'copied' is written byte-wise (a byte-wise write by 'memcpy')
struct copied { int *p; int n; };
// CHECK: disqualified struct punned: an object of 'punned' is converted from another type
struct punned { int *p; int n; };

void grow(struct vec *v, size_t n) {
  // CHECK: store group in grow at [[@LINE+1]]:3: data len (2 stores, last at [[@LINE+2]]:3)
  v->data = malloc(n * sizeof *v->data);
  v->len = n;
}

void reset(struct vec *v, struct text *t) {
  // An access through the object between the stores splits the group.
  // CHECK: store group in reset at [[@LINE+1]]:3: len (1 stores, last at [[@LINE+1]]:3)
  v->len = 0;
  // CHECK: store group in reset at [[@LINE+1]]:14: data (1 stores, last at [[@LINE+1]]:14)
  int x = 1; v->data = &x + (v->len > 0);
  // CHECK: store group in reset at [[@LINE+1]]:3: buf cap (2 stores, last at [[@LINE+2]]:3)
  t->buf = 0;
  t->cap = 0;
}

void others(struct taken *a, struct copied *b, const struct copied *c,
            long *raw) {
  int *where = &a->n;
  (void)where;
  memcpy(b, c, sizeof *b);
  struct punned *p = (struct punned *)raw;
  (void)p;
  a->p = b->p;
  a->n = b->n;
}
