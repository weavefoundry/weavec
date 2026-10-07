// RFC 0030 §16 (gate H3): -fdiagnostics-format=sarif crashes neither tool.
//
// weavec-cc renders a compile's diagnostics, those of -fweavec-diagnose
// among them, as that compile's SARIF document.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -fweavec-diagnose -fdiagnostics-format=sarif -c %s -o %t/bug.o -DBUG 2>&1 | FileCheck --check-prefix=SARIF %s
//
// weavec's runs create their SourceManager before Clang would attach the
// SARIF printer's document writer, so the tool refuses the flag.
// RUN: not %weavec %s -- -fdiagnostics-format=sarif 2>&1 | FileCheck --check-prefix=WEAVEC %s
// RUN: not %weavec --whole-program %s -- -fdiagnostics-format=sarif 2>&1 | FileCheck --check-prefix=WEAVEC %s
// RUN: not %weavec --extra-arg=-fdiagnostics-format=sarif %s -- 2>&1 | FileCheck --check-prefix=WEAVEC %s
// RUN: %weavec %s -- -fdiagnostics-format=sarif -fdiagnostics-format=clang -I%S/../WholeProgram/Inputs 2>&1 | FileCheck --check-prefix=LAST %s
#include "../Inputs/prelude.h"

// SARIF: "text": "use of 'p' after it was freed [weavec::use-after-free]"
// SARIF: "version": "2.1.0"
// SARIF-NOT: Stack dump
// WEAVEC: weavec: error: -fdiagnostics-format=sarif is not supported
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
  node_free(n);
  return 0;
}
#endif
