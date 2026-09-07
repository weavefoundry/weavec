// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
static void zap(char **a,int i,int j){free(a[i]);*a[j]=1;}
void run(void) { char *p = malloc(4); if (!p) return; char*q=malloc(4);if(!q){free(p);return;}char*items[2]={p,q};zap(items,0,1);free(q); }
