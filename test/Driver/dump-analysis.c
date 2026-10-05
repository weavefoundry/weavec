// --dump-analysis prints each analysed function's format-30 summary
// (RFC 0031 §6.1, *Summary format 30*). The format is a debugging aid and may
// change; this pins only its shape and the facts each function is about.
// RUN: %weavec --dump-analysis %s -- | FileCheck %s
// RUN: %weavec --ledger=%t.json %s --
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
// RUN: %weavec --help | FileCheck --check-prefix=HELP %s
#include "../Inputs/prelude.h"

struct s {
  int *buf;
};

// The release under `if (c)` is guarded by `c` being non-zero: the summary
// keys it by the parameter's zero test (RFC 0009; RFC 0031 *Pending cases
// and exit splitting*: no result class separates it, so it is `lossy`).
// CHECK-LABEL: function 'f':
// CHECK-NEXT: summary:
// CHECK-NEXT: always-returns
// CHECK-NEXT: release *param0->buf free lossy may when param 1 !=0
// CHECK-NOT: release
void f(struct s *p, int c) {
  int x = 0;
  int *a = &x;
  if (c)
    free(p->buf);
  use(a);
}

// A function with no effects has a summary of one line.
// CHECK-LABEL: function 'g':
// CHECK-NEXT: summary:
// CHECK-NEXT: always-returns
// CHECK-NOT: {{[a-z]}}
void g(void) {}

// The summary is the function's interface as inferred (RFC 0003): the
// unchecked `malloc` result stored in `gp` is a fresh object of the `free`
// family with an extent of 4 bytes that may be null (RFC 0007, RFC 0008);
// the result is a copy of `p->buf`, and every exit dereferenced `p`.
// CHECK-LABEL: function 'h':
// CHECK-NEXT: summary:
// CHECK-NEXT: always-returns
// CHECK-NEXT: result path param0->buf when null nonnull
// CHECK-NEXT: store global0 := fresh#0 free extent 4{{.*}} maybe-null
// CHECK-NEXT: nonnull-on null param0
// CHECK-NEXT: nonnull-on nonnull param0
// Reading `p->buf` is spatially proven by `p`'s Single default (RFC 0030
// §7.3), a lower bound of one `struct s`.
// LEDGER: "name": "h",
// LEDGER: "kind": "deref",
// LEDGER-NEXT: "line": [[@LINE+7]],
// LEDGER: "text": "p->buf",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "proven",
static int *gp;
int *h(struct s *p) {
  gp = malloc(4);
  return p->buf;
}

// A pointer made from an integer is a value of unknown provenance in the
// summary, not a raw one (RFC 0033 §2).
// CHECK-LABEL: function 'launder':
// CHECK-NEXT: summary:
// CHECK-NEXT: always-returns
// CHECK-NEXT: result unknown maybe-null when null nonnull
char *launder(char *r, unsigned long x) {
  r = (char *)x;
  return r;
}

// HELP: --dump-analysis
