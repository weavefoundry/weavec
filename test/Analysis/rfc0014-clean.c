// RUN: %weavec %s -- > %t 2>&1
// RUN: count 0 < %t
// RFC 0014: these complete, supported operations retain precise identities.
#include <stdlib.h>
#include <string.h>

static void drop(void *p) { free(p); }
static void keep(void *p) { (void)p; }
void (*unrelated)(void *) = drop;
static void invoke(void (*fn)(void *), void *data) { fn(data); }
void callback_clean(int *p) {
  void (*fn)(void *) = keep;
  invoke(fn, p);
  *p = 1;
  free(p);
}

static void release_same(int *p, int *q) {
  if (p == q)
    free(p);
}
void guarded(int *p, int *q) {
  int *saved = p;
  if (p != q) {
    release_same(saved, q);
    *saved = 1;
    free(saved);
  }
}

void pointer_copy(void) {
  int *p = malloc(sizeof *p), *q;
  if (!p)
    return;
  memcpy(&q, &p, sizeof p);
  *q = 1;
  free(q);
}

struct inner { int *p; };
struct outer { int tag; struct inner in; };
static void initialize(void *object) {
  struct inner *p = object;
  *p->p = 1;
}
void embedded(struct outer *p) {
  void *object = &p->in;
  initialize(object);
}
