/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { int a[8]; int *end=a+8; return (int)(end-a); }
