// RFC 0012, *Sized fields*, "Inference": the stores in vec.c witness
// `(struct vec.items, struct vec.cap, 4)` and nothing in the program refutes
// it, so a reader in another unit is checked against the count; `struct
// view.raw` is stored from a caller's pointer once, which refutes it.
//
// RUN: not %weavec --whole-program %s %S/Inputs/vec.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/vec.c -- -I%S/Inputs 2>/dev/null | FileCheck --check-prefix=DUMP %s
//
// Alone, nothing witnesses the pair: nothing is reported.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --allow-empty --check-prefix=ALONE %s
//
// The same through weavec-cc: the sidecar carries the witnesses and the
// refutations (format 8).
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/vec.c -o %t/vec.o -I%S/Inputs 2>&1 | count 0
// RUN: %weavec_cc -c %s -o %t/main.o -I%S/Inputs 2>&1 | count 0
// RUN: FileCheck --check-prefix=SIDECAR %s < %t/vec.o.weavec
// RUN: FileCheck --check-prefix=LOADS %s < %t/main.o.weavec
// RUN: not %weavec_cc %t/vec.o %t/main.o -o %t/prog 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "vec.h"

// DUMP: program:
// DUMP: sized-field 'struct vec.items' by 'struct vec.cap' * 4
// DUMP-NEXT: sized-field 'struct view.raw' by 'struct view.len' * 4
// DUMP-NEXT: unsized-field 'struct view.raw'

// SIDECAR: weavec-summaries 12
// SIDECAR-DAG: sized-field struct~vec.items struct~vec.cap 4
// SIDECAR-DAG: sized-field struct~view.raw struct~view.len 4
// SIDECAR-DAG: unsized-field struct~view.raw

// This unit looked the fields up without deciding anything: the link step
// knows to analyse it again once another unit witnesses the pair.
// LOADS-DAG: loads-field struct~vec.items
// LOADS-DAG: loads-field struct~view.raw

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
  // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:10: error: 'v->items[v->cap]' is out of bounds: 'v->cap' is the number of elements of 'v->items' [weavec::out-of-bounds]
  return v->items[v->cap] + x;
  // CHECK: vec.h:9:8: note: 'v->items' is declared here
}

// A loop to the count inclusive.
void zero(struct vec *v) {
  for (size_t i = 0; i <= v->cap; i++)
    // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:5: error: 'v->items[i]' may be out of bounds: 'i' may equal 'v->cap', the number of elements of 'v->items' [weavec::out-of-bounds]
    v->items[i] = 0;
}

// Refuted: `view_own` witnesses `(raw, len)`, `view_borrow` stores a pointer
// of unknown extent into `raw`; the refutation wins.
int view_last(struct view *w) { return w->raw[w->len]; }
