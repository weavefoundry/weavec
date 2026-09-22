// Link-only unit of 14_unknown_extern_frees_bug.c: the definition the analysis
// must not see, compiled as plain Clang so the executable links.
// FLAGS: -fno-weavec
#include <stdlib.h>
void consume_buffer(char *p) { free(p); }
