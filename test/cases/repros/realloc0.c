// Root-cause repro realloc0: a my_realloc wrapper (free when n == 0, else realloc) behind a
// strbuffer-style append. v0.10.0 reports only analysis-incomplete here (a control case).
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
static int append(buf_t *b, const char *d, size_t sz) {
    if (sz >= b->size - b->length) {
        size_t ns = b->length + sz + 1;
        char *nv;
        if (b->length > (size_t)-1 / 2 || sz > (size_t)-1 / 2) return -1;
        nv = my_realloc(b->value, ns);
        if (!nv) return -1;
        b->value = nv; b->size = ns;
    }
    memcpy(b->value + b->length, d, sz);
    b->length += sz;
    return 0;
}
int use(buf_t *b) {
    if (append(b, "x", 1) == -1) { free(b->value); return -1; }
    free(b->value);
    return 0;
}
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) { buf_t b = {0}; return use(&b) == 0 ? 0 : 1; }
