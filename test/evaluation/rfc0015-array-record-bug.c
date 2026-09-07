// RFC 0015: fixed bug/clean pair; see the evaluation manifest.
#include "../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
struct box { char *p; int n; };
void run(struct box *a) { struct box b[2]; memcpy(b,a,sizeof b); free(a[1].p);
  b[1].p[0]=1; // BUG: array-record
}
