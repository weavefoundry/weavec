// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void run(char *p, char *q) { char **a=malloc(2*sizeof *a); if(!a) return; a[0]=p; a[1]=q;
  char **b=realloc(a,4*sizeof *a); if(!b) { free(a); return; } free(p);
  b[0][0]=1; // BUG: array-resized
  free(b);
}
