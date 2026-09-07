// RFC 0015: selected cells retain state across unrelated updates.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);

void history(char **a) {
  free(a[0]); free(a[1]);
  // CHECK: rfc0015-elements.c:[[@LINE+1]]:3: error: use of 'a[0]' after it was freed [weavec::use-after-free]
  a[0][0]=1;
  // CHECK: rfc0015-elements.c:[[@LINE+1]]:3: error: 'a[0]' is freed twice [weavec::double-free]
  free(a[0]);
}
void saved(char **a, int i) {
  int old=i; free(a[i]); i=7;
  // CHECK: rfc0015-elements.c:[[@LINE+1]]:3: error: use of 'a[array-index(i)]' after it was freed [weavec::use-after-free]
  a[old][0]=1;
}
void initialization(char *p) {
  char *a[2]; a[0]=p;
  // CHECK: rfc0015-elements.c:[[@LINE+1]]:3: error: use of 'a[1]' before it was initialized [weavec::use-of-uninitialized]
  a[1][0]=1;
}
void omitted(char *p) {
  char *a[2]={p};
  // CHECK: rfc0015-elements.c:[[@LINE+1]]:3: error: dereference of 'a[1]', which is null [weavec::null-dereference]
  a[1][0]=1;
}
void copied(char **a) {
  char *b[2]; memcpy(b,a,sizeof b); free(a[0]);
  // CHECK: rfc0015-elements.c:[[@LINE+1]]:3: error: use of 'b[0]' after it was freed [weavec::use-after-free]
  b[0][0]=1;
}
void clean(char **a, char *p) {
  free(a[0]); a[1][0]=1; a[0]=p; a[0][0]=1;
  char *b[2]={p,p}; memmove(b,b,0); b[1][0]=1;
}
// CHECK: 6 errors generated.
