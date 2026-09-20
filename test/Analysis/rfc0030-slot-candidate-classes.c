// RFC 0030 §9.1, *Widened cases*: a summary's `effects` map is the union over
// its result classes, and it is the per-class entries that qualify a consume —
// `realloc` records `p: moved(free)` unguarded there and `when[n zero]` only on
// its null class. A candidate with no class-dependent consumption keeps no
// classes at all, so joining it into that summary folds the classes away and
// would leave the bare union behind as a must-fact. It is a widened consume:
// the join claims it on paths the other candidate does not consume on, so no
// caller may make a definite finding from it.
//
// The slot below holds two targets: a `realloc` wrapper and one that only ever
// returns null. Releasing the old pointer after the call is at most *possible*
// (it is a real release only when the wrapper ran with a zero size), never a
// certain use after move.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=NOTDEF %s
#include <stdlib.h>

struct hooks {
  void *(*reallocate)(void *, size_t);
};

static void *wrap_realloc(void *pointer, size_t size) {
  return realloc(pointer, size);
}

static void *always_fail(void *pointer, size_t size) {
  (void)pointer;
  (void)size;
  return NULL;
}

// CHECK: :[[@LINE+9]]:{{[0-9]+}}: warning: use of 'buffer' after it may have been moved
// NOTDEF-NOT: after it was moved
static char *grow(struct hooks *h, char *buffer, size_t size) {
  char *grown = (char *)h->reallocate(buffer, size);
  if (grown != NULL) {
    return grown;
  }
  // The wrapper releases `buffer` only for a zero size, and `always_fail`
  // never releases it, so this is a possible double free and no more.
  free(buffer);
  return NULL;
}

int main(void) {
  struct hooks growing;
  struct hooks failing;
  char *p = (char *)malloc(16);
  if (p == NULL) {
    return 1;
  }
  growing.reallocate = wrap_realloc;
  failing.reallocate = always_fail;
  p = grow(&growing, p, 32);
  if (p == NULL) {
    return 1;
  }
  free(p);
  (void)failing;
  return 0;
}
