// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void run(char **a) { char *b[2]; memcpy(b,a,sizeof b); free(a[0]); b[1][0]=1; }
