// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
static void zap(char *a, char *b) { *b = 1; free(a); }
void run(void) { char *p = malloc(4); if (!p) return; zap(p, p); }
