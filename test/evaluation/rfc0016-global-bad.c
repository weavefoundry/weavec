// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
char*shared;static void zap(char*a){free(a);*shared=1;} // BUG: global
void run(void) { char *p = malloc(4); if (!p) return; shared=p;zap(p); }
