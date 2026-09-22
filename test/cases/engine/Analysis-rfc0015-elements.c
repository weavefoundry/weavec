// Engine pin converted from test/Analysis/rfc0015-elements.c; markers are the v0.10.0 golden diagnostics.
// RFC 0015: selected cells retain state across unrelated updates.
#include "Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);

void history(char **a) {
  free(a[0]); free(a[1]);
  a[0][0]=1; // BUG: use-after-free
  free(a[0]); // BUG: double-free
}
void saved(char **a, int i) {
  int old=i; free(a[i]); i=7;
  a[old][0]=1; // BUG: use-after-free
}
void initialization(char *p) {
  char *a[2]; a[0]=p;
  a[1][0]=1; // BUG: use-of-uninitialized
}
void omitted(char *p) {
  char *a[2]={p};
  a[1][0]=1; // BUG: null-dereference
}
void copied(char **a) {
  char *b[2]; memcpy(b,a,sizeof b); free(a[0]);
  b[0][0]=1; // BUG: use-after-free
}
void clean(char **a, char *p) {
  free(a[0]); a[1][0]=1; a[0]=p; a[0][0]=1;
  char *b[2]={p,p}; memmove(b,b,0); b[1][0]=1;
}
