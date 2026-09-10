/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void compact(char *p) { char *out=p; while(*p) { if(*p!=' ') *out++=*p; ++p; } *out=0; }
int main(void) { char a[]="a b"; compact(a); return a[1]; }
