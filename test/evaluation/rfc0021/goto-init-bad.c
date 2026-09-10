/* RFC 0021: frozen traversal acceptance case. */
#include <stddef.h>
#include <stdlib.h>
int main(void) { goto done; int x=1; done: return x; }
