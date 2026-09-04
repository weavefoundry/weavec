// RFC 0010, *Across translation units*: a ref/unref pair inferred in one
// unit is a retain and a share release in another, and the count field it
// releases through is a known count for the leak rule there.
//
// RUN: not %weavec --whole-program %s %S/Inputs/counted.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/counted.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=DUMP %s
//
// Alone, the calls are unchecked boundaries: nothing is reported.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"
#include "counted.h"

// DUMP: program:
// DUMP: function 'counted_new': stores{} returns{fresh(free), null}
// DUMP-NEXT: function 'counted_ref': param 0 *.rc: read,written; stores{} returns{copy param 0} requires{param 0} increments{param 0 *.rc}
// DUMP-NEXT: function 'counted_unref': param 0: freed(free),share; param 0 *.rc: read,written; stores{} returns{} requires{param 0} decrements{param 0 *.rc} counts{param 0 *.rc}
// DUMP: count-field 'struct counted.rc'

// Clean: a share taken and given back.
int balanced(void) {
  struct counted *a = counted_new();
  if (!a)
    return -1;
  struct counted *b = counted_ref(a);
  counted_unref(b);
  counted_unref(a);
  return 0;
}

int twice(void) {
  struct counted *a = counted_new();
  if (!a)
    return -1;
  counted_unref(a);
  // CHECK: rfc0010-shares.c:[[@LINE+1]]:3: error: 'a' is released twice [weavec::double-free]
  counted_unref(a);
  return 0;
}

// The count is known from the other unit: a lost share is a leak here.
struct list {
  struct counted *head;
};
void lost(struct list *l) {
  struct counted *p = l->head;
  // CHECK: rfc0010-shares.c:[[@LINE+1]]:3: warning: 'p' is leaked [weavec::leak]
  counted_ref(p);
}

// ALONE: warning: call to 'counted_new' is not checked
// ALONE-NOT: error:
// CHECK: 1 warning and 1 error generated.
