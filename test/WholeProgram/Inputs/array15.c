// RFC 0015: selected effects, final ranges, returned storage and traversals.
#include "array15.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void array15_drop(char **a, int i) { free(a[i]); }
void array15_copy(char **d, char **s, size_t n) { memcpy(d,s,n*sizeof *d); }
void array15_compact(char **a) { memmove(a,a+1,2*sizeof *a); a[2]=NULL; }
char **array15_clone(char **source, size_t n) {
  char **b=malloc(n*sizeof *b); if (!b) return NULL;
  memcpy(b,source,n*sizeof *b); return b;
}
void array15_clear(char **a, size_t n) {
  for (size_t i=0; i<n; ++i) { free(a[i]); a[i]=NULL; }
}
void array15_fill(char **a, int n) {
  for (int i=0; i<n; ++i) a[i]=malloc(4);
}
