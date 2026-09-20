// Unit of examples/unresolved-unknown-callee-link.c: 'consume' frees its argument.
#include <stdlib.h>

void consume(char *p) { free(p); }
