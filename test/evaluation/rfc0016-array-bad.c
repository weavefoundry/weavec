// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
static void zap(char **a,int i,int j){free(a[i]);*a[j]=1;} // BUG: array
void run(void) { char *p = malloc(4); if (!p) return; char*items[2]={p,p};zap(items,0,1); }
