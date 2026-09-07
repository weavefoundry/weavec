// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void run(char **a, char **b, size_t n) { if(n<3) return; memcpy(b,a,n*sizeof *b); n=0; free(a[2]); b[1][0]=1; }
