// RFC 0003: a call to a function with neither a definition in this
// translation unit nor ownership annotations is a checking boundary. RFC 0030
// §5.1 makes it an unknown callee: no diagnostic, a Call site whose temporal
// facet is `unresolved(unknown-callee)` with a suggestion (a fix-it when the
// declaration is the program's own), and what it may have freed or kept is
// unresolved after it. A third-party header installed as a system header is
// no platform header (§5.2), so its functions are unknown too. RFC 0030
// removes `annotation-required` and `--strict-externs`.
// RUN: %weavec %s -- -isystem %S/Inputs 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>
#include <vendor.h>

void mystery(void *p);
int pure(int x);
void *maker(void);
void annotated(void *WEAVEC_BORROWED p);

void f(char *p) {
  mystery(p);
  mystery(p);
  pure(1);
  // `annotated` only borrows `p`, but `mystery` may have freed it.
  annotated(p);
  // No fix-it into a system header. The detail is the unknown callee's own
  // suggestion (RFC 0031 §5.1), not the earlier unknown code (`mystery`)
  // that may have freed `p`.
  vendor_touch(p);
  // `use` borrows its argument, but the argument points into memory an
  // unknown callee made: unresolved, not trusted to `use`'s contract (RFC
  // 0031 *Implementation amendments*, "Memory the analysis knows nothing
  // about").
  use(maker());
}

// An unsafe region does not make a callee known.
void vouched(char *p) {
  WEAVEC_UNSAFE { mystery(p); }
}

// CHECK-NOT: warning:
// CHECK: 0 errors, 0 warnings
