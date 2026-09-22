// Unit of kinds/attr-alloc-size.c: a definition WeaveC does not see, larger than declared.
// FLAGS: -fno-weavec
#include <stdlib.h>

void *grab(size_t n) { return malloc(n + 16); }
