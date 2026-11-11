// RFC 0042 §10: a guard a typed function dropped because it believes typed
// memory is kept, in verify mode, as a monitor: a guarded function that
// breaks typed memory is reported as `weavec.proven`, and so is a use after
// free in typed code.
// RUN: %weavec_cc -O1 -g -fweavec-typed %s -o %t
// RUN: %t lie
// RUN: %t stale
// RUN: %weavec_cc -O1 -g -fweavec-typed -fweavec-checks=verify %s -o %t.verify
// RUN: not --crash %t.verify lie 2>&1 | FileCheck %s --check-prefix=LIE
// RUN: not --crash %t.verify stale 2>&1 | FileCheck %s --check-prefix=STALE
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t cap; };
struct box { int *value; };

int get(const struct vec *v, size_t i) {
  // LIE: weavec: weavec.proven: heap-buffer-overflow at {{.*}}typed-verify.c:[[@LINE+1]]
  return v->data[i];
}

int contents(const struct box *b) {
  // STALE: weavec: weavec.proven: heap-use-after-free at {{.*}}typed-verify.c:[[@LINE+1]]
  return *b->value;
}

WEAVEC_GUARDED void lie(struct vec *v) { v->cap = 12; }

WEAVEC_GUARDED int main(int argc, char **argv) {
  struct vec v = {calloc(8, sizeof(int)), 8};
  struct box b = {calloc(1, sizeof(int))};
  if (argc > 1 && !strcmp(argv[1], "lie")) {
    lie(&v);
    (void)get(&v, 10);
  }
  if (argc > 1 && !strcmp(argv[1], "stale")) {
    free(b.value);
    (void)contents(&b);
  }
  return 0;
}
