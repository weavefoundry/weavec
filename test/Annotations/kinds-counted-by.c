// RFC 0030 §7.2, row 1: WEAVEC_COUNTED_BY(n) and WEAVEC_SIZED_BY(n) on
// parameters and fields are `counted(n)`, and `sized(n)` for void and
// character pointees. The two macros are synonyms. `n` names any sibling
// parameter (in any position) or sibling field; a name that resolves to
// nothing, or to a non-integer, is an invalid annotation and the kind is
// dropped.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s
#include <weavec.h>

#if WEAVEC_H_VERSION_MINOR < 9
#error "weavec.h 0.9 spells WEAVEC_COUNTED_BY"
#endif

typedef unsigned long size_t;

struct buf {
  // CHECK: field buf.items: counted(.cap scale 1 plus 0) nullable [declared, declared, shape annotation]
  int *WEAVEC_COUNTED_BY(cap) items;
  // CHECK: field buf.bytes: sized(.cap scale 1 plus 0) nullable [declared, declared, shape annotation]
  char *WEAVEC_COUNTED_BY(cap) bytes;
  // CHECK: field buf.raw: sized(.cap scale 1 plus 0) nullable [declared, declared, shape annotation]
  void *WEAVEC_SIZED_BY(cap) raw;
  // CHECK: field buf.legacy: counted(.cap scale 1 plus 0) nullable [declared, declared, shape annotation]
  long *WEAVEC_SIZED_BY(cap) legacy;
  size_t cap;
  // CHECK: problem [[@LINE+1]]:35: 'nowhere' in WEAVEC_COUNTED_BY does not name a parameter or field
  int *WEAVEC_COUNTED_BY(nowhere) lost;
  // CHECK: problem [[@LINE+1]]:32: 'lost' in WEAVEC_COUNTED_BY is not an integer parameter or field
  int *WEAVEC_COUNTED_BY(lost) pointer_count;
};

// The count may follow the pointer: names resolve in any position.
// CHECK: param fill 0 'out': sized(param 1 scale 1 plus 0) nullable [declared, declared, shape annotation]
void fill(char *WEAVEC_COUNTED_BY(len) out, size_t len);
// CHECK: param sum 1 'values': counted(param 0 scale 1 plus 0) nullable [declared, declared, shape annotation]
long sum(int n, const int *WEAVEC_SIZED_BY(n) values);
// CHECK: problem [[@LINE+1]]:48: 'missing' in WEAVEC_COUNTED_BY does not name a parameter or field
void bad_name(char *WEAVEC_COUNTED_BY(missing) p, size_t n);
// CHECK: problem [[@LINE+1]]:42: 'q' in WEAVEC_COUNTED_BY is not an integer parameter or field
void bad_type(char *WEAVEC_COUNTED_BY(q) p, char *q);
// CHECK: problem [[@LINE+1]]:43: 'x' is declared WEAVEC_COUNTED_BY(n) but is not a pointer
void not_pointer(int WEAVEC_COUNTED_BY(n) x, int n);

// A redeclaration that renames the count keeps the kind (the annotation
// was resolved on the declaration that wrote it).
// CHECK: param renamed 0 'data': counted(param 1 scale 1 plus 0) nullable [declared, declared, shape annotation]
void renamed(int *WEAVEC_COUNTED_BY(n) data, int n);
void renamed(int *data, int count) { (void)data; (void)count; }

void use(char *c, int *i) {
  fill(c, 1);
  (void)sum(1, i);
  bad_name(c, 1);
  bad_type(c, c);
  not_pointer(1, 1);
}
// CHECK-NOT: problem
