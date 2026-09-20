// RFC 0030 §5.6: every emitted function is analysed, `static inline`
// functions of user headers included (`--analyze-headers` is removed); each
// unit analyses its own copy. A header function CodeGen never emits (one no
// code of the unit references) is not, and definitions in system headers
// never are.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec_cc -fsyntax-only %s 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/buggy-header.h"

// CHECK: buggy-header.h:4:3: error: 'p' is freed twice [weavec::double-free]
// CHECK-NOT: buggy-header.h:9:
// CHECK: 1 error generated.
void fine(int *p) { buggy(p); }
