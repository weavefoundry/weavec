/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { char *p=malloc(4); if(!p) return 0; p[0]=1; goto cleanup; cleanup: free(p); return 0; }
