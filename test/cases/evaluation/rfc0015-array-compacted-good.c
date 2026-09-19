// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
// CLEAN
// UNITS: Inputs/array15.c
#include "Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
#include "Inputs/array15.h"
void run(char **a) { char *old=a[1]; array15_compact(a); free(old); a[1][0]=1; free(a[2]); }
