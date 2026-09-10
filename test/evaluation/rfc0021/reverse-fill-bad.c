/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void fill(char *p, unsigned n) { while(n) p[n--]=1; }
int main(void) { char a[4]; fill(a,4); return a[1]; }
