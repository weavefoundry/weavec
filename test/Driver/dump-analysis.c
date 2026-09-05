// --dump-analysis prints the inferred places and exit state per function.
// The format is a debugging aid and may change; this pins only its shape.
// RUN: %weavec --dump-analysis %s -- | FileCheck %s
// RUN: %weavec --help | FileCheck --check-prefix=HELP %s
#include "../Inputs/prelude.h"

struct s {
  int *buf;
};

// The release under `if (c)` is guarded by `c` being non-zero, in the state
// and in the summary's `when` clause (RFC 0009).
// CHECK-LABEL: function 'f':
// CHECK-NEXT: places:{{.*}}p (param, unknown){{.*}}a (local, mutable)
// CHECK-NEXT: lifetimes:{{.*}}caller
// CHECK-NEXT: exit: moved{p->buf@[[@LINE+6]]:{{[0-9]+}} freed(free) when[c positive|negative]} loans{} aliases{} raw{} owned{}
// CHECK-NEXT: summary: p->buf: freed(free) when[c positive|negative]; stores{} returns{}
void f(struct s *p, int c) {
  int x = 0;
  int *a = &x;
  if (c)
    free(p->buf);
  use(a);
}

// CHECK-LABEL: function 'g':
// CHECK: exit: moved{} loans{} aliases{} raw{} owned{}
// CHECK-NEXT: summary: stores{} returns{}
void g(void) {}

// The summary is the function's interface as inferred (RFC 0003); owned
// resources and their release family show in the exit state (RFC 0007), and
// what is known about nullness in `nulls{}` (RFC 0008): the unchecked
// `malloc` result may be null, so the store may be null too, and reading
// `p->buf` requires `p` (and proves it non-null from there on).
// CHECK-LABEL: function 'h':
// CHECK: exit: moved{} loans{} aliases{} raw{} owned{gp@[[@LINE+4]]:{{[0-9]+}} allocated free} nulls{p@[[@LINE+5]]:{{[0-9]+}} nonnull, gp@[[@LINE+4]]:{{[0-9]+}} maybe-null}
// CHECK-NEXT: summary: p->buf: read; stores{gp = fresh(free) extent=4, gp = null} returns{copy p->buf} requires{p}
static int *gp;
int *h(struct s *p) {
  gp = malloc(4);
  return p->buf;
}

// Raw places show their kind, the raw component says why (RFC 0004), and
// `raw` is a value source in the summary.
// CHECK-LABEL: function 'launder':
// CHECK-NEXT: places:{{.*}}r (param, raw)
// CHECK: exit: moved{} loans{} aliases{} raw{r@[[@LINE+3]]:{{[0-9]+}} integer-cast} owned{}
// CHECK-NEXT: summary: stores{} returns{raw}
char *launder(char *r, unsigned long x) {
  r = (char *)x;
  return r;
}

// HELP: --dump-analysis
