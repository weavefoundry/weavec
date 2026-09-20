// RFC 0030 §7.2 precedence: WEAVEC_* annotations (level 1) outrank
// ecosystem attributes (level 2), which outrank the LibrarySpec row
// (level 3) and attributes in system headers (level 4). Shape and
// nullability are resolved separately. A conflict within one level is an
// invalid annotation, and the weaker kind (the join) is used.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s
#include <weavec.h>

// The annotation's shape wins; `static` still gives the nullability.
// CHECK: param mixed 1 'p': counted(param 0 scale 1 plus 0) nonnull [declared, declared, shape annotation, nullability ecosystem]
void mixed(int n, int p[static 4] WEAVEC_COUNTED_BY(n));
// The annotation's nullability wins over the attribute's.
// CHECK: param lenient 0 'p': unknown nullable [declared, nullability annotation]
void lenient(int *WEAVEC_NULLABLE p __attribute__((nonnull)));
// Two redeclarations that disagree: the join, and a problem.
// CHECK: param torn 0 'p': unknown nullable [declared, shape annotation]
// CHECK-NEXT: problem [[@LINE+2]]:37: conflicting kinds for 'p': counted(param 1 scale 1 plus 0) nullable and counted(param 2 scale 1 plus 0) nullable
void torn(int *WEAVEC_COUNTED_BY(n) p, int n, int m);
void torn(int *WEAVEC_COUNTED_BY(m) p, int n, int m);

void use(int *p) {
  mixed(4, p);
  lenient(p);
  torn(p, 1, 1);
}
