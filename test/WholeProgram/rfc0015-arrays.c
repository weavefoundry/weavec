// RFC 0015: arrays across units in the whole-program analysis.
// RUN: not %weavec --whole-program %s %S/Inputs/array15.c -- 2>&1 | FileCheck %s
#include "Inputs/array15.h"

// RFC 0031 §6.1: format 30 has no array copy, fill or release forms; they
// are stores and releases over `[*]` steps, with an element range where one
// is known (`array15_drop`, `array15_clear`), and constant indices as byte
// offsets (`array15_compact`, *Summary paths*).

// Format 30 has no per-element copy or release forms: a copy or a clone
// is known per element through the context the call asks of `array15.c`
// (RFC 0031 §7 *Amendment (cross-unit contexts)*), but the element a clear
// released is not: `cleared` is `unresolved(may-alias-released)`, never
// proven, where the old engine reported a use after free
// (the retired golden comparison of RFC 0030, *Lit tests*).

void selected(char **a) {
  array15_drop(a,0); array15_drop(a,1);
  // CHECK: rfc0015-arrays.c:[[@LINE+1]]:3: error: use of 'a[0]' after it was freed [weavec::use-after-free]
  a[0][0]=1;
}
void copied(char **a, char **b) {
  array15_copy(b,a,3); free(a[2]);
  // CHECK: rfc0015-arrays.c:[[@LINE+2]]:3: error: use of 'b[2]' after it was freed [weavec::use-after-free]
  // CHECK: rfc0015-arrays.c:[[@LINE-2]]:24: note: freed here (through 'a[2]')
  b[2][0]=1;
}
void compacted(char **a) {
  char *old=a[1]; array15_compact(a); free(old);
  // CHECK: rfc0015-arrays.c:[[@LINE+1]]:3: error: use of 'a[0]' after it was freed [weavec::use-after-free]
  a[0][0]=1;
}
void returned(char **a) {
  char **b=array15_clone(a,3); if (!b) return; free(a[2]);
  // CHECK: rfc0015-arrays.c:[[@LINE+1]]:3: error: use of 'b[2]' after it was freed [weavec::use-after-free]
  b[2][0]=1; free(b);
}
void cleared(char **a) {
  char *old=a[1]; array15_clear(a,3);
  old[0]=1;
}
void clean(char **a, char **b) {
  array15_copy(b,a,3); free(a[2]); b[1][0]=1;
  char *c[3]={NULL}; array15_fill(c,3); array15_clear(c,3);
  free(c[0]); free(c[1]); free(c[2]);
}
int main(void) { return 0; }
// CHECK: 4 errors generated.
