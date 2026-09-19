// Root-cause repro realloc4: the realloc emulation without the n == 0 branch; v0.10.0
// reports 'b->value' freed twice (false).
// intended (RFC 0030 section 9.1, gate S7): no finding. The wrapper's free is keyed to the
// result class that returns it, so the caller's result test selects it exactly.
// CLEAN
// ASAN
#include <stdlib.h>
#include <string.h>
typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);
static malloc_fn do_malloc = malloc;
static free_fn do_free = free;
typedef struct { char *value; size_t length, size; } buf_t;
static void *my_realloc(void *ptr, size_t old, size_t n) {
    void *m;
    m = malloc(n);
    if (m && ptr) { memcpy(m, ptr, old < n ? old : n); free(ptr); }
    return m;
}
static int append(buf_t *b, char c) {
    if (1 >= b->size - b->length) {
        size_t ns = b->size * 2 > b->length + 2 ? b->size * 2 : b->length + 2;
        char *nv;
        if (b->size > 1000 || b->length > 1000) return -1;
        nv = my_realloc(b->value, b->size, ns);
        if (!nv) return -1;
        b->value = nv; b->size = ns;
    }
    b->value[b->length++] = c;
    return 0;
}
void twice(buf_t *b) {
    append(b, 'a');
    append(b, 'b');
}
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) { buf_t b = {0}; twice(&b); free(b.value); return 0; }
