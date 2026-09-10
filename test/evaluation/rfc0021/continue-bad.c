/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void fill(char *p,unsigned n) { for(unsigned i=0;i<n;++i) { if(i==2) continue; p[i]=1; } }
int main(void) { char a[4]; fill(a,4); return a[2]; }
