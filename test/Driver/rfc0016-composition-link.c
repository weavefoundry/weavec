// RFC 0016: the compiler driver serializes the callee's summary and checks
// the composition at link.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/../WholeProgram/Inputs/rfc0016-callee.c -o %t/callee.o
// RUN: %weavec_cc -c %s -o %t/caller.o
// RUN: not %weavec_cc %t/caller.o %t/callee.o -o %t/program 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: %weavec --dump-record=%t/callee.o.weavec | FileCheck %s --check-prefix=FORMAT
// RUN: not test -f %t/program
//
// RFC 0031 §7 *Amendment (cross-unit contexts)*: at link the caller asks
// `release_then_write` for the context of its aliased arguments; the
// callee's unit runs again to serve it and reports the use of `b` after
// `free(a)` there.
#include "../Inputs/prelude.h"
void release_then_write(char *, char *);
int main(void) {
  char *p = malloc(4); if (p) release_then_write(p, p);
  return 0;
}
// FORMAT: "format": 29,
// FORMAT: "name": "release_then_write",
// FORMAT: "effects": "returns always\neffect release p0* when=-:- family=free\nstore p1* when=-:- :: int lo=1 hi=1\nwrites p1*\n",
// LINK: rfc0016-callee.c:4:4: error: use of 'b' after it was freed [weavec::use-after-free]
// LINK: rfc0016-callee.c:3:3: note: freed here (through 'a')
// LINK: rfc0016-callee.c:2:6: note: called from another unit with related pointer arguments
// LINK: 1 error generated.
