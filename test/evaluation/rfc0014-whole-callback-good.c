// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
void invoke(void (*)(void *), void *);
static void keep(void *p) { (void)p; }
void clean(int *p) { invoke(keep, p); *p = 1; free(p); }
