/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void advance(char **p,unsigned n) { *p+=n; }
int main(void) { char a[4]={0}; char *p=a; advance(&p,4); return *p; }
