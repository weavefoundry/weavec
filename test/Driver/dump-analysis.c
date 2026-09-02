// --dump-analysis prints the inferred places and exit state per function.
// The format is a debugging aid and may change; this pins only its shape.
// RUN: %weavec --dump-analysis %s -- | FileCheck %s
// RUN: %weavec --help | FileCheck --check-prefix=HELP %s
#include "../Inputs/prelude.h"

struct s {
  int *buf;
};

// CHECK-LABEL: function 'f':
// CHECK-NEXT: places:{{.*}}p (param, unknown){{.*}}a (local, mutable)
// CHECK-NEXT: lifetimes:{{.*}}caller
// CHECK-NEXT: exit: moved{p->buf@[[@LINE+6]]:{{[0-9]+}} freed} loans{} aliases{}
// CHECK-NEXT: summary: p->buf: freed; stores{} returns{}
void f(struct s *p, int c) {
  int x = 0;
  int *a = &x;
  if (c)
    free(p->buf);
  use(a);
}

// CHECK-LABEL: function 'g':
// CHECK: exit: moved{} loans{} aliases{}
// CHECK-NEXT: summary: stores{} returns{}
void g(void) {}

// The summary is the function's interface as inferred (RFC 0003).
// CHECK-LABEL: function 'h':
// CHECK: summary: p->buf: read; stores{gp = fresh} returns{copy p->buf}
static int *gp;
int *h(struct s *p) {
  gp = malloc(4);
  return p->buf;
}

// HELP: --dump-analysis
