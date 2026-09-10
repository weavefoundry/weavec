/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
static void compact(char *p) { char *out=p; while(*p) { *out++=*p; *out++=*p; ++p; } *out=0; }
int main(void) { char a[]="abc"; compact(a); return a[1]; }
