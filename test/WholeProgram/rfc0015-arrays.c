// RFC 0015: the tool and the compiler's serialized link analysis agree.
// RUN: not %weavec --whole-program %s %S/Inputs/array15.c -- 2>&1 | FileCheck %s
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/array15.c -o %t/library.o 2>&1 | count 0
// RUN: %weavec_cc -Wno-weavec-annotation-required -c %s -o %t/caller.o 2>&1 | count 0
// RUN: FileCheck --check-prefix=SIDECAR %s < %t/library.o.weavec
// RUN: not %weavec_cc %t/library.o %t/caller.o -o %t/program 2>&1 | FileCheck %s
#include "Inputs/array15.h"

// SIDECAR: weavec-summaries 16
// RFC 0017: memcpy's element count is the wrapped byte product divided by 8.
// SIDECAR-DAG: array-copy param 0 * from param 1 * dest-begin 0 source-begin 0 count expr u64,c,8;u64,v,706172616d2032;u64,mul;u64,c,8;u64,div scale 1 plus 0 bytes 8 view pointer definite when cmp u64,c,8;u64,v,706172616d2032;u64,mul;u64,c,8;u64,div in u64:0-2305843009213693951
// SIDECAR-DAG: array-copy result * from param 0 * dest-begin 0 source-begin 0 count expr u64,c,8;u64,v,706172616d2031;u64,mul;u64,c,8;u64,div scale 1 plus 0 bytes 8 view pointer definite when cmp u64,c,8;u64,v,706172616d2031;u64,mul;u64,c,8;u64,div in u64:0-2305843009213693951
// SIDECAR-DAG: array-release param 0 * begin 0 count param 1 scale 1 plus 0 cleared definite
// SIDECAR-DAG: array-fill param 0 * count param 1 scale 1 plus 0 malloc 4 definite

void selected(char **a) {
  array15_drop(a,0); array15_drop(a,1);
  // CHECK: rfc0015-arrays.c:[[@LINE+1]]:3: error: use of 'a[0]' after it was freed [weavec::use-after-free]
  a[0][0]=1;
}
void copied(char **a, char **b) {
  array15_copy(b,a,3); free(a[2]);
  // CHECK: rfc0015-arrays.c:[[@LINE+1]]:3: error: use of 'b[2]' after it was freed [weavec::use-after-free]
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
  // CHECK: rfc0015-arrays.c:[[@LINE+1]]:3: error: use of 'old' after it was freed [weavec::use-after-free]
  old[0]=1;
}
void clean(char **a, char **b) {
  array15_copy(b,a,3); free(a[2]); b[1][0]=1;
  char *c[3]={NULL}; array15_fill(c,3); array15_clear(c,3);
  free(c[0]); free(c[1]); free(c[2]);
}
int main(void) { return 0; }
// CHECK: 5 errors generated.
