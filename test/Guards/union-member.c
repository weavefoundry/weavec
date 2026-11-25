// RFC 0054 §3.3: a tagged union's member used while the tag says another
// stops with `union-member`, in both builds, and a store of a new value to
// the tag clears the union.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t.fast
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: %t.fast | FileCheck %s --check-prefix=OK
// RUN: %t.full | FileCheck %s --check-prefix=OK
// RUN: not --crash %t.fast read 2>&1 | FileCheck %s --check-prefix=READ
// RUN: not --crash %t.full read 2>&1 | FileCheck %s --check-prefix=READ
// RUN: not --crash %t.fast write 2>&1 | FileCheck %s --check-prefix=WRITE
// RUN: not --crash %t.fast masked 2>&1 | FileCheck %s --check-prefix=MASKED
#include <stdio.h>
#include <string.h>
#include <weavec.h>

enum kind { KIND_NUMBER, KIND_TEXT };
struct value {
  enum kind kind;
  union {
    double number WEAVEC_WHEN(KIND_NUMBER);
    const char *WEAVEC_NULL_TERMINATED text WEAVEC_WHEN(KIND_TEXT);
  } as WEAVEC_TAGGED_BY(kind);
};
struct masked {
  unsigned char tt;
  union { long i WEAVEC_WHEN(3); double d WEAVEC_WHEN(19); } value WEAVEC_TAGGED_BY(tt, 0x1f);
};

__attribute__((noinline)) char first(const struct value *v) {
  // READ: weavec: union-member at {{.*}}union-member.c:[[@LINE+1]]:{{[0-9]+}}: 'text' read while 'kind' is 0
  return v->as.text[0];
}
__attribute__((noinline)) void setNumber(struct value *v, double d) {
  // WRITE: weavec: union-member at {{.*}}union-member.c:[[@LINE+1]]:{{[0-9]+}}: 'number' written while 'kind' is 1
  v->as.number = d;
}
__attribute__((noinline)) long integer(const struct masked *m) {
  // MASKED: weavec: union-member at {{.*}}union-member.c:[[@LINE+1]]:{{[0-9]+}}: 'i' read while 'tt' is 19
  return m->value.i;
}
__attribute__((noinline)) int retag(struct value *v) {
  v->kind = KIND_NUMBER; // clears the union: the text is gone
  return v->as.number == 0.0;
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  struct value v = {.kind = KIND_TEXT, .as.text = "hello"};
  struct masked m = {.tt = 3 | 0x40, .value.i = 7};
  if (argc > 1 && strcmp(argv[1], "read") == 0) {
    v.kind = KIND_NUMBER;
    return first(&v);
  }
  if (argc > 1 && strcmp(argv[1], "write") == 0) {
    setNumber(&v, 1.0);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "masked") == 0) {
    m.tt = 19;
    return (int)integer(&m);
  }
  // OK: h 7 cleared 1
  printf("%c %ld cleared %d\n", first(&v), integer(&m), retag(&v));
  return 0;
}
