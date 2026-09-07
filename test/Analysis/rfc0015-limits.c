// RFC 0015: exhausted selection budgets preserve earlier temporal evidence.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
void bounded(char **a) {
  free(a[0]);
  use(a[1]);
  use(a[2]);
  use(a[3]);
  use(a[4]);
  use(a[5]);
  use(a[6]);
  use(a[7]);
  use(a[8]);
  use(a[9]);
  use(a[10]);
  use(a[11]);
  use(a[12]);
  use(a[13]);
  use(a[14]);
  use(a[15]);
  use(a[16]);
  use(a[17]);
  use(a[18]);
  use(a[19]);
  use(a[20]);
  use(a[21]);
  use(a[22]);
  use(a[23]);
  use(a[24]);
  use(a[25]);
  use(a[26]);
  use(a[27]);
  use(a[28]);
  use(a[29]);
  use(a[30]);
  use(a[31]);
  // CHECK: warning: analysis is incomplete: array element limit reached [weavec::analysis-incomplete]
  use(a[32]);
  // CHECK: rfc0015-limits.c:[[@LINE+1]]:3: error: use of 'a[0]' after it was freed [weavec::use-after-free]
  a[0][0]=1;
}
void weak(char **a, char *p, int i, int j) {
  char *old=a[0]; free(a[0]);
  // CHECK: warning: analysis is incomplete: unresolved array element selection [weavec::analysis-incomplete]
  // CHECK: warning: analysis is incomplete: unresolved array element update [weavec::analysis-incomplete]
  a[i*j]=p;
  // CHECK: rfc0015-limits.c:[[@LINE+1]]:3: error: use of 'old' after it was freed [weavec::use-after-free]
  old[0]=1;
}
