// RFC 0010, *Across translation units*: a ref/unref pair inferred in one
// unit is a retain and a share release in another, and the count field it
// releases through is a known count for the leak rule there.
//
// RUN: not %weavec --whole-program %s %S/Inputs/counted.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/counted.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=DUMP %s
//
// Alone, the calls are into unknown code: nothing is reported.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"
#include "counted.h"

// The object engine does not yet infer RFC 0010 reference-count functions
// (`++c->rc` / `if (--c->rc == 0) free(c)`): `counted_unref`'s summary
// possibly releases its argument, `counted_ref` returns it, and no count
// field is exported (RFC 0031 §5.5, §6.1; the retired golden comparison of RFC 0030,
// *Lit tests*). A call whose count is known asks `counted.c` for its
// context (RFC 0031 §7 *Amendment (cross-unit contexts)*): there the count
// decides the release.
// DUMP: program:
// DUMP: function 'counted_new':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result null when null
// DUMP-NEXT: result fresh#0 free extent 16 {{.*}}when nonnull
// DUMP: function 'counted_ref':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result path param0 when nonnull
// DUMP: function 'counted_unref':
// DUMP-NEXT: always-returns
// DUMP-NEXT: release *param0 free may when always
// DUMP-NEXT: release *param0->name free may when always

// Clean: a share taken and given back.
int balanced(void) {
  struct counted *a = counted_new();
  if (!a)
    return -1;
  struct counted *b = counted_ref(a);
  // The count is 2 here, 1 after this call, which releases nothing.
  counted_unref(b);
  counted_unref(a);
  return 0;
}

int twice(void) {
  struct counted *a = counted_new();
  if (!a)
    return -1;
  counted_unref(a);
  // The first call released `a` (its count was 1); this one reads its count.
  // CHECK: rfc0010-shares.c:[[@LINE+1]]:17: error: use of 'a' after it was freed [weavec::use-after-free]
  counted_unref(a);
  return 0;
}

// With the count known from the other unit, a lost share is a leak here
// (RFC 0010). Without the count (above), `counted_ref` only returns its
// argument and the leak is not reported (a lost finding, listed in
// the retired golden comparison of RFC 0030, *Lit tests*).
struct list {
  struct counted *head;
};
void lost(struct list *l) {
  struct counted *p = l->head;
  counted_ref(p);
}

// Alone, the calls into the other unit are unknown code (RFC 0030 §5.1).
// ALONE-NOT: {{warning|error}}:
// ALONE: 0 errors, 0 warnings
// CHECK-NOT: {{warning|error}}:
// CHECK: 1 error generated.
