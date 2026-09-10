/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { char a[8]; char *end=a+8; return a<end; }
