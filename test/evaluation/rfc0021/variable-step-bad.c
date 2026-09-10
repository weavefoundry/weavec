/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static int scan(const unsigned char *p,unsigned n) { unsigned i=0; while(i<n) { unsigned k=p[i]; if(k==0) return 0; i+=k; } return p[i]; }
int main(void) { unsigned char a[4]={5,0,2,0}; return scan(a,4); }
