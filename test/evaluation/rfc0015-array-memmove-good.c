// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void run(char **a) { char *old=a[1]; memmove(a+1,a,2*sizeof *a); free(old); a[1][0]=1; }
