// RFC 0016: contextual diagnostics belong to the callee operation.
#include "heap.h"
void zap(char*a,char*b){*b=1;free(a);}
