// RFC 0016: callee operation diagnostics from upstream context requests.
//
// RFC 0031 §7 *Amendment (cross-unit contexts)*: `bad` passes one object
// twice, asks the callee's unit for that context, and the callee's unit
// reports what the context finds there, with a note that another unit made
// the call.
//
// RUN: not %weavec --whole-program %s %S/Inputs/rfc0016-callee.c -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
void release_then_write(char *, char *);
void write_then_release(char *, char *);
void replace_related(char **, char **);
void bad(void) {
  char *p = malloc(4); if (p) release_then_write(p, p);
}
void good(void) {
  char *p = malloc(4); if (!p) return;
  write_then_release(p, p);
  p = malloc(4); if (!p) return;
  replace_related(&p, &p); if (p) *p = 1; free(p);
}
// CHECK: rfc0016-callee.c:4:4: error: use of 'b' after it was freed [weavec::use-after-free]
// CHECK: rfc0016-callee.c:3:3: note: freed here (through 'a')
// CHECK: rfc0016-callee.c:2:6: note: called from another unit with related pointer arguments
// CHECK: 1 error generated.
// CHECK: weavec: program program: {{.*}}; 1 error, 0 warnings
// The callee's row records the violation the context found.
