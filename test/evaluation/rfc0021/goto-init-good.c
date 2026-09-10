/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { int x=1; goto done; done: return x; }
