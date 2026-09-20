// The definition rfc0030-link-declarations.c declares WEAVEC_BORROWED: it
// frees its parameter (probe 38's 38_extern_impl.c).
#include <stdlib.h>
void inspect(char *p) { free(p); }
