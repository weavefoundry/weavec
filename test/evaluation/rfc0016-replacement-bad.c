// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
static void zap(char **a, char **b) { char *saved=*b; free(*a); *a=malloc(4); *saved=1; } // BUG: replacement
void run(void) { char *p = malloc(4); if (!p) return; zap(&p,&p);free(p); }
