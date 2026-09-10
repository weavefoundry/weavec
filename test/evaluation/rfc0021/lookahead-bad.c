/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static int scan(const char *p,unsigned n) { unsigned i=0; while(i<n) { if(p[i]==p[i+1]) return 1; ++i; } return 0; }
int main(void) { char a[4]={1,2,3,4}; return scan(a,4); }
