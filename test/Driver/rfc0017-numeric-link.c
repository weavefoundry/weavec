// RFC 0017: the unit record carries numeric facts and access intervals in
// its summaries.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/../WholeProgram/Inputs/rfc0017-numeric.c -o %t/callee.o
// RUN: %weavec_cc -c %s -o %t/caller.o
// RUN: %weavec --dump-record=%t/callee.o.weavec | FileCheck %s --check-prefix=FORMAT
// RUN: not %weavec_cc -fweavec-link=analyze %t/caller.o %t/callee.o -o %t/program 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: not test -f %t/program
// RUN: not %weavec --no-runtime --whole-program --ledger=%t/program.json %s %S/../WholeProgram/Inputs/rfc0017-numeric.c -- 2>&1
// RUN: FileCheck %s --check-prefix=LEDGER < %t/program.json
#include "../Inputs/prelude.h"
unsigned char narrow(unsigned);
void narrow_out(unsigned, unsigned char *);
void reverse_outputs(unsigned *, unsigned *);
void release_numeric_pointer(void *);
void put_at(char *, int);
// `narrow_out(256, &n)` stores 0: format 30 carries the stored integer as
// the interval of the conversion, [0, 255] (RFC 0031 §6.1), but the call
// asks `narrow_out` for the context of its constant argument (RFC 0031 §7
// *Amendment (cross-unit contexts)*), where it is 0.
// LINK: rfc0017-numeric-link.c:[[@LINE+4]]:3: error: 'q[0]' is out of bounds: index 0 of an object of 0 bytes [weavec::out-of-bounds]
void output_value(void) {
  unsigned char n = 1; narrow_out(256, &n);
  char *q = malloc(n); if (!q) return;
  q[0] = 1; free(q);
}
// `reverse_outputs(&n, &n)` writes `*b = 1` and then `*a = 2` through the
// same cell, so `n` is 2 after the call: the first test is false and the
// second reads `q` after it was freed.
// LINK-NOT: rfc0017-numeric-link.c:[[@LINE+7]]:
// LINK: rfc0017-numeric-link.c:[[@LINE+7]]:16: error: use of 'q' after it was freed [weavec::use-after-free]
void ordered_outputs(void) {
  unsigned n;
  reverse_outputs(&n, &n);
  int *q = malloc(sizeof *q); if (!q) return;
  release_numeric_pointer(q);
  if (n == 1) *q = 1; // Clean: the last aliased write stored two.
  if (n == 2) *q = 1;
}
// `put_at(a, -1)` writes before `a` in the call's context (RFC 0031
// *Implementation amendments*, "Stores past the caller's object"), and
// `narrow(256)` returns 0 in its.
// LINK: rfc0017-numeric-link.c:[[@LINE+3]]:21: error: 'put_at' requires 'a' before its start [weavec::out-of-bounds]
// LINK: rfc0017-numeric-link.c:[[@LINE+4]]:3: error: 'p[0]' is out of bounds: index 0 of an object of 0 bytes [weavec::out-of-bounds]
int main(void) {
  char a[4]; put_at(a, -1);
  char *p = malloc(narrow(256)); if (!p) return 0;
  p[0] = 1; free(p);
  return 0;
}
// The access in `put_at` itself stays unresolved in its own unit (RFC 0017
// §5: `counted(i + 1)` does not cover a signed index).
// LEDGER: "name": "put_at",
// LEDGER: "text": "p[i]",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "unresolved",
// LEDGER-NEXT: "reason": "unknown-extent",
// FORMAT: "format": 31,
// FORMAT: "name": "narrow",
// FORMAT: "effects": "returns always\nresult classes=zero,positive :: int lo=0 hi=255\n",
// FORMAT: "name": "narrow_out",
// FORMAT: "effects": "returns always\nstore p1* when=-:- :: int lo=0 hi=255\nwrites p1*\n",
// FORMAT: "name": "put_at",
// FORMAT: "effects": "returns always\nstore p0*[] when=-:- elements=p1@1@0,p1@1@1 :: int lo=1 hi=1\nwrites p0*\n",
// LINK: 4 errors generated.
