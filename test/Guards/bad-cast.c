// RFC 0054 §2.2: a checked conversion of a safe C function stops with
// `bad-cast` when no object of the target type is at the address: a heap
// block of another type (whose paragraph names it), a global, a recorded
// local; container_of and plain scalars are checked the same way, and
// memory with no record is believed.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t.fast
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: %t.fast | FileCheck %s --check-prefix=OK
// RUN: %t.full | FileCheck %s --check-prefix=OK
// RUN: not --crash %t.fast heap 2>&1 | FileCheck %s --check-prefix=HEAP
// RUN: not --crash %t.full heap 2>&1 | FileCheck %s --check-prefix=HEAP
// RUN: not --crash %t.fast global 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not --crash %t.fast local 2>&1 | FileCheck %s --check-prefix=LOCAL
// RUN: not --crash %t.fast container 2>&1 | FileCheck %s --check-prefix=CONTAINER
// RUN: not --crash %t.fast scalar 2>&1 | FileCheck %s --check-prefix=SCALAR
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct shape { int kind; };
struct circle { struct shape base; double radius; };
struct square { struct shape base; double side; };
struct link { struct link *next; };
struct item { int key; struct link link; };
struct other { long a; struct link link; };
struct holder { char *name; };

struct square board = {.base.kind = 2, .side = 4};

__attribute__((noinline)) double radius(struct shape *s) {
  // HEAP: weavec: bad-cast at {{.*}}bad-cast.c:[[@LINE+4]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'struct circle'
  // HEAP-NEXT: #0 {{.*}} in radius
  // HEAP: is 0 bytes inside the 16-byte heap object of type 'struct square' at
  // GLOBAL: weavec: bad-cast at {{.*}}bad-cast.c:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'struct circle'
  return ((struct circle *)s)->radius;
}
__attribute__((noinline)) double radiusOf(void *context) {
  // LOCAL: weavec: bad-cast at {{.*}}bad-cast.c:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'struct circle'
  struct circle *c = context;
  return c->radius;
}
__attribute__((noinline)) int key(struct link *l) {
  // CONTAINER: weavec: bad-cast at {{.*}}bad-cast.c:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'struct item'
  return ((struct item *)((char *)l - offsetof(struct item, link)))->key;
}
__attribute__((noinline)) long word(void *context) {
  // SCALAR: weavec: bad-cast at {{.*}}bad-cast.c:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'long'
  return *(long *)context;
}
__attribute__((noinline)) double keep(void *context) {
  struct square here = {.base.kind = 2, .side = 3};
  void *again = &here; // its address becomes a `void *`: a frame record
  return radiusOf(context != NULL ? context : again);
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "";
  struct circle *c = calloc(1, sizeof *c);
  struct item *it = calloc(1, sizeof *it);
  long *n = calloc(1, sizeof *n);
  c->radius = 2;
  it->key = 7;
  *n = 9;
  if (strcmp(what, "heap") == 0) {
    struct square *q = calloc(1, sizeof *q);
    return (int)radius(&q->base);
  }
  if (strcmp(what, "global") == 0)
    return (int)radius(&board.base);
  if (strcmp(what, "local") == 0)
    return (int)keep(NULL);
  if (strcmp(what, "container") == 0) {
    struct other *o = calloc(1, sizeof *o);
    return key(&o->link);
  }
  if (strcmp(what, "scalar") == 0) {
    struct holder *h = calloc(1, sizeof *h);
    return (int)word(h);
  }
  // OK: 2 7 9
  printf("%g %d %ld\n", radius(&c->base), key(&it->link), word(n));
  free(c);
  free(it);
  free(n);
  return 0;
}
