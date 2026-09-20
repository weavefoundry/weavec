// Root-cause repro realloc1: the same wrapper behind a grow helper whose failure path the
// caller cleans up. v0.10.0 reports nothing (a control case).
// intended (RFC 0030 section 9.1, gate S7): no finding. The wrapper's free is keyed to the
// result class that returns it, so the caller's result test selects it exactly.
// CLEAN
// ASAN
#include <stdlib.h>
#include <string.h>
typedef struct { char *value; size_t length, size; } buf_t;
static void *my_realloc(void *ptr, size_t n) {
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}
static int grow(buf_t *b) {
    char *nv = my_realloc(b->value, 16);
    if (!nv) return -1;
    b->value = nv;
    return 0;
}
int use(buf_t *b) {
    if (grow(b) == -1) { free(b->value); return -1; }
    free(b->value);
    return 0;
}
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) { buf_t b = {0}; return use(&b) == 0 ? 0 : 1; }
