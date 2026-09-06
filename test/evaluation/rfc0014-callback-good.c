// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
static void keep(void *p) { (void)p; }
static void drop(void *p) { free(p); }
static void invoke(void (*fn)(void *), void *p) { fn(p); }
void clean(int *p) { invoke(keep, p); *p = 1; free(p); }
