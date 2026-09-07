// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
static void drop(char **a, size_t n) { for(size_t i=0;i<n;++i) free(a[i]); }
void run(char **a) { drop(a,3); a[3][0]=1; }
