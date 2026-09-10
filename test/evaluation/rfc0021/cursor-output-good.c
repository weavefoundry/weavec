/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static char *fill(char *p,unsigned n) { char *end=p+n; while(p<end) *p++=1; return p; }
int main(void) { char a[4]; char *end=fill(a,4); return end[-1]; }
