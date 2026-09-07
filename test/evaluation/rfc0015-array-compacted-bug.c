// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
#include "../WholeProgram/Inputs/array15.h"
void run(char **a) { char *old=a[1]; array15_compact(a); free(old);
  a[0][0]=1; // BUG: array-compacted
}
