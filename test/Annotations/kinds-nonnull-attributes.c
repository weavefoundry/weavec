// RFC 0030 §7.2, row 7: `nonnull` (all pointer parameters), `nonnull(i, ...)`
// (the listed ones), `nonnull` on a parameter, `returns_nonnull`, `_Nonnull`
// and `_Nullable` declare nullability at the ecosystem level (level 4 in a
// system header, where a LibrarySpec row outranks them).
// RUN: %weavec --dump-kinds %s -- -isystem %S/Inputs | FileCheck %s
#include <vendor-kinds.h>

// CHECK: param all 0 'a': unknown nonnull [declared, nullability ecosystem]
// CHECK: param all 1 'b': unknown nonnull [declared, nullability ecosystem]
void all(int *a, int *b) __attribute__((nonnull));
// CHECK-NOT: param some 0
// CHECK: param some 1 'b': unknown nonnull [declared, nullability ecosystem]
void some(int *a, int *b) __attribute__((nonnull(2)));
// CHECK: param each 0 'a': unknown nonnull [declared, nullability ecosystem]
// CHECK: param each 1 'b': unknown nonnull [declared, nullability ecosystem]
// CHECK: param each 2 'c': unknown nullable [declared, nullability ecosystem]
void each(int *a __attribute__((nonnull)), int *_Nonnull b, int *_Nullable c);
// CHECK: result fresh: single nonnull [default, lower-bound, nullability ecosystem]
int *fresh(void) __attribute__((returns_nonnull));

void use(int *p) {
  all(p, p);
  some(p, p);
  each(p, p, p);
  (void)fresh();
  vendor_fill(p);
}
// A system header's attribute is level 4.
// CHECK-NOT: vendor_fill
