// RFC 0030 §7.2, row 3: WEAVEC_STRING on a pointer parameter, field or
// return type is `nul-terminated`: a zero element lies at or after the
// pointer within its object. It takes no argument and has no extent term.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s
#include <weavec.h>

struct entry {
  // CHECK: field entry.key: nul-terminated nullable [declared, shape annotation]
  const char *WEAVEC_STRING key;
  // CHECK: field entry.wide: nul-terminated nullable [declared, shape annotation]
  const int *WEAVEC_STRING wide;
};

// CHECK: param length 0 's': nul-terminated nullable [declared, shape annotation]
unsigned long length(const char *WEAVEC_STRING s);
// CHECK: result label: nul-terminated nullable [declared, shape annotation]
const char *WEAVEC_STRING label(int code);
// With WEAVEC_NONNULL, the nullability is declared too.
// CHECK: param greet 0 'name': nul-terminated nonnull [declared, shape annotation, nullability annotation]
void greet(const char *WEAVEC_STRING WEAVEC_NONNULL name);
// CHECK: problem [[@LINE+1]]:35: 'c' is declared WEAVEC_STRING but is not a pointer
int not_pointer(int WEAVEC_STRING c);

// CHECK: problem [[@LINE+1]]:27: 'global' is declared WEAVEC_STRING but only parameters, fields and results take it
const char *WEAVEC_STRING global;

unsigned long use(const char *s) {
  greet(s);
  return length(label(0)) + (unsigned long)not_pointer(1) + length(global);
}
