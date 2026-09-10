/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void fill(char *p, unsigned n) { unsigned i=0; while(i<n) { p[i]=1; ++i; } }
int main(void) { char a[4]; fill(a,4); return a[3]; }
