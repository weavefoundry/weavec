// RFC 0030 §7.6: the candidates `count(d) == f + c` and `bytes(d) == f + c`
// (c in {0, 1}) of a struct's pointer and integer fields are proposed for
// Houdini, except for a struct some write the analysis cannot see reaches: a
// field's address taken, a byte-wise write, a conversion from another type.
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

// A declared kind needs no candidate.
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
  v->data = malloc(n * sizeof *v->data);
  v->len = n;
}

void reset(struct vec *v, struct text *t) {
  v->len = 0;
  int x = 1; v->data = &x + (v->len > 0);
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
