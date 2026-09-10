/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { char *p=malloc(4); if(!p) return 0; char *end=p+4; free(p); return (int)(end-p); }
