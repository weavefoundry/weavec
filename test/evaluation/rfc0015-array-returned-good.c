// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
#include "../WholeProgram/Inputs/array15.h"
void run(char **a) { char **b=array15_clone(a,3); if(!b) return; free(a[2]); b[1][0]=1; free(b); }
