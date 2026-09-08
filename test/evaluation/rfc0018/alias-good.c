// RFC 0018 fixed checked-code evaluation: separate allocations.
#include <stdlib.h>
static void release_write(int *p,int *q) { free(p); *q=1; }
int main(void) { int *p=malloc(sizeof *p),*q=malloc(sizeof *q); if (!p || !q) { free(p); free(q); return 0; } release_write(p,q); free(q); return 0; }
