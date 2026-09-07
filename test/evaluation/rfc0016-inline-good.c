// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
void run(void) { char *p = malloc(4); if (!p) return; *p=1;free(p); }
