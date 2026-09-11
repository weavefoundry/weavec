// RFC 0016: the compiler driver serializes requests/results and checks at link.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec-cc -c %S/../WholeProgram/Inputs/rfc0016-callee.c -o %t/callee.o
// RUN: %weavec-cc -c %s -o %t/caller.o
// RUN: not %weavec-cc %t/caller.o %t/callee.o -o %t/program 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: FileCheck %s --check-prefix=FORMAT < %t/callee.o.weavec
// RUN: not test -f %t/program
#include "../Inputs/prelude.h"
void release_then_write(char *, char *);
int main(void) {
  char *p = malloc(4); if (p) release_then_write(p, p);
  return 0;
}
// FORMAT: weavec-summaries 18
// FORMAT: accepts-memory-contexts
// LINK: rfc0016-callee.c:4:4: error: use of 'b' after it was freed [weavec::use-after-free]
// LINK: 1 error generated.
