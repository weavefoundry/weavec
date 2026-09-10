/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static unsigned fill(char *p,unsigned n) { unsigned i=0; while(i<n) { if(i==2) break; p[i++]=1; } return i; }
int main(void) { char a[4]; fill(a,4); return a[3]; }
