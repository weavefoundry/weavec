// RFC 0030 §7.2, row 2: WEAVEC_ENDED_BY(q) on a pointer parameter or field
// is `ended-by(q)`: `[p, q)` lies in one object. `q` names a sibling
// pointer parameter or field, in any position; anything else is an invalid
// annotation and the kind is dropped.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s
#include <weavec.h>

struct span {
  // CHECK: field span.first: ended-by(.last) nullable [declared, declared, shape annotation]
  const int *WEAVEC_ENDED_BY(last) first;
  const int *last;
  // CHECK: problem [[@LINE+1]]:36: 'size' in WEAVEC_ENDED_BY is not a pointer parameter or field
  const int *WEAVEC_ENDED_BY(size) other;
  int size;
};

// CHECK: param total 0 'p': ended-by(param 1) nullable [declared, declared, shape annotation]
long total(const int *WEAVEC_ENDED_BY(end) p, const int *end);
// CHECK: param backwards 1 'p': ended-by(param 0) nullable [declared, declared, shape annotation]
long backwards(const int *end, const int *WEAVEC_ENDED_BY(end) p);
// CHECK: problem [[@LINE+1]]:44: 'n' in WEAVEC_ENDED_BY is not a pointer parameter or field
long counted(const int *WEAVEC_ENDED_BY(n) p, int n);
// CHECK: problem [[@LINE+1]]:47: 'stop' in WEAVEC_ENDED_BY does not name a parameter or field
long nowhere(const int *WEAVEC_ENDED_BY(stop) p, const int *end);

long use(const int *a, const int *b) {
  return total(a, b) + backwards(b, a) + counted(a, 1) + nowhere(a, b);
}
// CHECK-NOT: problem
