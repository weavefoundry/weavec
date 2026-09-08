// RFC 0017: format 13 carries numeric expressions and access intervals.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec-cc -c %S/../WholeProgram/Inputs/rfc0017-numeric.c -o %t/callee.o
// RUN: %weavec-cc -c %s -o %t/caller.o
// RUN: FileCheck %s --check-prefix=FORMAT < %t/callee.o.weavec
// RUN: not %weavec-cc %t/caller.o %t/callee.o -o %t/program 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: not test -f %t/program
#include "../Inputs/prelude.h"
unsigned char narrow(unsigned);
void narrow_out(unsigned, unsigned char *);
void reverse_outputs(unsigned *, unsigned *);
void release_numeric_pointer(void *);
void put_at(char *, int);
void output_value(void) {
  unsigned char n = 1; narrow_out(256, &n);
  char *q = malloc(n); if (!q) return;
  q[0] = 1; free(q);
}
void ordered_outputs(void) {
  unsigned n;
  reverse_outputs(&n, &n);
  int *q = malloc(sizeof *q); if (!q) return;
  release_numeric_pointer(q);
  if (n == 1) *q = 1; // Clean: the last aliased write stored two.
  if (n == 2) *q = 1;
}
int main(void) {
  char a[4]; put_at(a, -1);
  char *p = malloc(narrow(256)); if (!p) return 0;
  p[0] = 1; free(p);
  return 0;
}
// FORMAT: weavec-summaries 15
// FORMAT-DAG: numeric result value
// FORMAT-DAG: requires-extent 0 param 1 scale 1 plus 1 start param 1 scale 1 plus 0
// LINK-DAG: error: 'put_at' requires 'a' before its start [weavec::out-of-bounds]
// LINK-DAG: error: 'p[0]' is out of bounds: index 0 of an object of 0 bytes [weavec::out-of-bounds]
// LINK-DAG: error: 'q[0]' is out of bounds: index 0 of an object of 0 bytes [weavec::out-of-bounds]
// LINK-DAG: error: use of 'q' after it was freed [weavec::use-after-free]
// LINK: 4 errors generated.
