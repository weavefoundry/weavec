// Root-cause repro realloc2: a realloc emulation through static malloc/free function
// pointers frees ptr only when the new block exists; v0.10.0 drops the guard on the local 'm'
// and reports 'b->value' freed twice at the second append (false).
// intended (RFC 0030 sections 9.1 and 9.3, gate S7): the closed slots resolve to malloc and
// free, the free is keyed to a nonnull result, and there is no finding.
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
    if (n == 0) { if (ptr) (*do_free)(ptr); return NULL; }
    m = (*do_malloc)(n);
    if (m && ptr) { memcpy(m, ptr, old < n ? old : n); (*do_free)(ptr); }
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
