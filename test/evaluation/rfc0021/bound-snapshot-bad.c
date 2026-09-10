/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { char a[4]={0}; unsigned n=4; char *end=a+n; n=8; return end[n-4]; }
