// RFC 0016: callee operation diagnostics from upstream context requests.
// RUN: not %weavec --strict-externs --whole-program %s %S/Inputs/rfc0016-callee.c -- 2>&1 | FileCheck %s
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
// CHECK: 1 error generated.
