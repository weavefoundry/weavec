// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
static void zap(char **a, char **b) { free(*a); *a=malloc(4); if(*b)**b=1; }
void run(void) { char *p = malloc(4); if (!p) return; zap(&p,&p);free(p); }
