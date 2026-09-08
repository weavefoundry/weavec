// RFC 0018 fixed checked-code evaluation: sufficient early-exit contract.
#include <stdlib.h>
static void fill(char *p, unsigned n) { for (unsigned i=0; i<n; ++i) { if (p[i]==42) break; p[i]=1; } }
int main(void) { char *p=calloc(4,1); if (!p) return 0; fill(p,4); free(p); return 0; }
