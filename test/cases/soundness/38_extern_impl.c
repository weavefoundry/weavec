// Second unit of 38_lying_annotation_bug.c: the definition that contradicts the
// WEAVEC_BORROWED declaration; the link step must verify it (RFC 0030 section 13.2).
#include <stdlib.h>
void inspect(char *p) { free(p); }
