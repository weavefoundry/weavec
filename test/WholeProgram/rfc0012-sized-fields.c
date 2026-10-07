// RFC 0012, *Sized fields*, "Inference": the stores in vec.c witness
// `(struct vec.items, struct vec.cap, 4)` and nothing in the program refutes
// it, so the old engine checked a reader in another unit against the count;
// `struct view.raw` is stored from a caller's pointer once, which refutes it.
//
// RFC 0031 §6.1 and §7: the object engine exports no sized-field facts (the
// record's `sizedFields` and `sizedFieldLoads` are gone), and it infers
// counted-field invariants only for records a unit defines in its main file
// (*Implementation amendments*, "Counted-field invariants"): `struct vec` is
// a header's, which other units' stores would have to confirm at link (RFC
// 0030 §7.6, A3, not built), so the readers here are
// `unresolved(unknown-extent)`: never proven, never a definite
// `out-of-bounds` (the retired golden comparison of RFC 0030, *Lit tests*). This test
// keeps asserting that no access is proven or reported.
//
// RUN: %weavec --whole-program %s %S/Inputs/vec.c -- -I%S/Inputs 2>&1 | FileCheck %s
//
// Alone, nothing witnesses the pair: nothing is reported.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --allow-empty --check-prefix=ALONE %s
#include "../Inputs/prelude.h"
#include "vec.h"

// CHECK-NOT: out-of-bounds
// CHECK: 0 errors, 0 warnings

// ALONE-NOT: error:
// ALONE-NOT: out-of-bounds

// Clean: within the count.
int sum(struct vec *v) {
  int total = 0;
  for (size_t i = 0; i < v->n; i++)
    total += v->items[i];
  return total;
}

// The count is `cap`, not `n` (`vec_push` writes `n` beside a store into
// `items`, so the pair `(items, n)` is not refuted, but no store witnesses
// it either): `v->items[v->cap]` is one past the end, `v->items[v->n]` is
// undecided (`n <= cap` is not known here).
int last(struct vec *v) {
  int x = v->items[v->n];
  // With the pair, a checked facet (RFC 0030 §3.3); now not proven.
  return v->items[v->cap] + x;
}

// A loop to the count inclusive.
void zero(struct vec *v) {
  for (size_t i = 0; i <= v->cap; i++)
    v->items[i] = 0;
}

// Refuted: `view_own` witnesses `(raw, len)`, `view_borrow` stores a pointer
// of unknown extent into `raw`; the refutation wins.
int view_last(struct view *w) { return w->raw[w->len]; }
