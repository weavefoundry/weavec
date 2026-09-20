// RFC 0016: paired compositional call evaluation.
// UNITS: Inputs/rfc0016-cross-bad.c
#include "Inputs/heap.h"
void zap(char*,char*);
void run(void) { char *p = malloc(4); if (!p) return; zap(p,p);}
