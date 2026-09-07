// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
static void fill(char **a, int n) { for(int i=0;i<n;++i) a[i]=malloc(4); }
void run(void) { char *a[3]; fill(a,3); for(int i=0;i<3;++i) free(a[i]);
  a[2][0]=1; // BUG: array-fill
}
