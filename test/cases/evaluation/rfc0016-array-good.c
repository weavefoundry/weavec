// RFC 0016: paired compositional call evaluation.
// CLEAN
// RFC 0030 §2.6: `zap` also gets the generic (authoritative) pass, which
// cannot tell `a[i]` from `a[j]`: a possible use-after-free warning at the
// callee (the temporal facet is `unresolved(may-released)`); the context
// run this caller requests is clean.
// ALLOW: use-after-free
#include "Inputs/heap.h"
static void zap(char **a,int i,int j){free(a[i]);*a[j]=1;}
void run(void) { char *p = malloc(4); if (!p) return; char*q=malloc(4);if(!q){free(p);return;}char*items[2]={p,q};zap(items,0,1);free(q); }
