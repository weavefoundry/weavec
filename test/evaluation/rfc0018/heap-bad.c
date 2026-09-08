// RFC 0018 fixed checked-code evaluation: memory initialization.
#include <stdlib.h>
int main(void) { int *p=malloc(sizeof *p); if (!p) return 0; int x=*p; free(p); return x; }
