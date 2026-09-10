/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static unsigned fill(char *p,unsigned n) { unsigned i=0; while(i<n) { if(i==2) break; p[i++]=1; } return i; }
int main(void) { char a[4]; unsigned n=fill(a,4); if(n!=2) return 0; return a[n-1]; }
