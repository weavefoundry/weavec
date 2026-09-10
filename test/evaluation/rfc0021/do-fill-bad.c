/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void fill(char *p, unsigned n) { unsigned i=0; do { p[i]=1; ++i; } while(i<n); }
int main(void) { char a[1]; fill(a+1,0); return 0; }
