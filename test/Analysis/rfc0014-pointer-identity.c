// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RFC 0014: actual callbacks, pointer predicates, complete memory copies.
#include <stdlib.h>
#include <string.h>

static void keep(void *p) { (void)p; }
static void drop(void *p) { free(p); }
static void invoke(void (*fn)(void *), void *p) { fn(p); }
void (*unrelated)(void *) = keep;

void callback_bad(int *p) {
  invoke(drop, p);
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  *p = 1;
}

static void release_same(int *p, int *q) {
  if (p == q)
    free(p);
}
void equality_bad(int *p) {
  release_same(p, p);
  // CHECK: error: 'p' is freed twice [weavec::double-free]
  free(p);
}

void copied_pointer_bad(int *p) {
  int *q;
  memcpy(&q, &p, sizeof p);
  free(p);
  // CHECK: error: use of 'q' after it was freed [weavec::use-after-free]
  *q = 1;
}

void partial(int **dest, int **source) {
  // CHECK: warning: analysis is incomplete: unsupported memory copy of pointer-containing storage [weavec::analysis-incomplete]
  memcpy(dest, source, 1);
}

struct first { int *p; };
struct second { int tag; int *p; };
static void release_field(void *object) {
  struct first *p = object;
  free(p->p);
}
void incompatible(struct second *p) {
  // CHECK: warning: analysis is incomplete: incompatible or unknown object view at call [weavec::analysis-incomplete]
  release_field(p);
}
