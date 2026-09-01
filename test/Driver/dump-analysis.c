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
// CHECK-NEXT: exit: moved{p->buf@[[@LINE+5]]:{{[0-9]+}} freed} loans{} aliases{}
void f(struct s *p, int c) {
  int x = 0;
  int *a = &x;
  if (c)
    free(p->buf);
  use(a);
}

// CHECK-LABEL: function 'g':
// CHECK: exit: moved{} loans{} aliases{}
void g(void) {}

// HELP: --dump-analysis
