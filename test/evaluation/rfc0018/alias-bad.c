// RFC 0018 fixed checked-code evaluation: interacting aliases.
#include <stdlib.h>
static void release_write(int *p,int *q) { free(p); *q=1; }
int main(void) { int *p=malloc(sizeof *p); if (!p) return 0; release_write(p,p); return 0; }
