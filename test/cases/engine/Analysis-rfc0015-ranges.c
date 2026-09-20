// Engine pin converted from test/Analysis/rfc0015-ranges.c; markers are the v0.10.0 golden diagnostics.
// RFC 0015: symbolic snapshots, overlap, complete initialization and cleanup.
#include "Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);

void changed_count(char **a, char **b, size_t n) {
  if (n<3) return;
  memcpy(b,a,n*sizeof *a); n=0; free(a[2]);
  b[2][0]=1; // BUG: use-after-free
}
void overlap_left(char **a) {
  char *old=a[1]; memmove(a,a+1,2*sizeof *a); free(old);
  a[0][0]=1; // BUG: use-after-free
}
void overlap_right(char **a) {
  char *old=a[1]; memmove(a+1,a,2*sizeof *a); free(old);
  a[2][0]=1; // BUG: use-after-free
}
void clean(void) {
  char *a[4]; for (int i=0;i<4;++i) a[i]=malloc(4);
  for (int i=0;i<4;++i) { free(a[i]); a[i]=NULL; }
  free(a[0]); free(a[3]);
}
