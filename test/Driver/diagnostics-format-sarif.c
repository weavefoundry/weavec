// RFC 0030 §16 (gate H3): -fdiagnostics-format=sarif crashes neither tool.
//
// weavec-cc renders a compile's diagnostics, WeaveC's among them, as that
// compile's SARIF document. The link step's re-analysis renders text
// whatever format the recorded command asked for, and the link relays it.
// RUN: rm -rf %t && mkdir -p %t
// RUN: not %weavec_cc -fdiagnostics-format=sarif -c %s -o %t/bug.o -DBUG 2>&1 | FileCheck --check-prefix=SARIF %s
// RUN: %weavec_cc -fdiagnostics-format=sarif -c %S/../WholeProgram/Inputs/node.c -o %t/node.o -I%S/../WholeProgram/Inputs > %t/node.log 2>&1
// RUN: %weavec_cc -fdiagnostics-format=sarif -c %s -o %t/main.o -I%S/../WholeProgram/Inputs > %t/main.log 2>&1
// RUN: not %weavec_cc %t/node.o %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
//
// weavec's runs create their SourceManager before Clang would attach the
// SARIF printer's document writer, so the tool refuses the flag and points
// to the ledger's SARIF instead.
// RUN: not %weavec %s -- -fdiagnostics-format=sarif 2>&1 | FileCheck --check-prefix=WEAVEC %s
// RUN: not %weavec --whole-program %s -- -fdiagnostics-format=sarif 2>&1 | FileCheck --check-prefix=WEAVEC %s
// RUN: not %weavec --extra-arg=-fdiagnostics-format=sarif %s -- 2>&1 | FileCheck --check-prefix=WEAVEC %s
// RUN: %weavec %s -- -fdiagnostics-format=sarif -fdiagnostics-format=clang -I%S/../WholeProgram/Inputs 2>&1 | FileCheck --check-prefix=LAST %s
#include "../Inputs/prelude.h"

// SARIF: "text": "use of 'p' after it was freed [weavec::use-after-free]"
// SARIF: "version": "2.1.0"
// SARIF-NOT: Stack dump
// WEAVEC: weavec: error: -fdiagnostics-format=sarif is not supported; write the ledger as SARIF with --ledger=<path> --ledger-format=sarif
// WEAVEC-NOT: Stack dump
// LAST: weavec: {{.*}}diagnostics-format-sarif.c: {{[0-9]+}} sites:
// LAST-NOT: "version"
// LAST-NOT: error:

#ifdef BUG
int use_after_free(void) {
  char *p = malloc(1);
  free(p);
  return p[0];
}
#else
#include "node.h"

int main(void) {
  struct node *n = node_new();
  node_free(n);
  // LINK: diagnostics-format-sarif.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  node_free(n);
  // LINK-NOT: "version"
  return 0;
}
#endif
