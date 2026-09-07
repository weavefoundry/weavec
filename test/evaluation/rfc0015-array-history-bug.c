// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void run(char **a) { free(a[0]); free(a[1]);
  a[0][0]=1; // BUG: array-history
}
