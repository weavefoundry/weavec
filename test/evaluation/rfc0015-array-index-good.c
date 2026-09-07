// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void run(char **a) { int i=0; free(a[i]); i=1; a[i][0]=1; }
