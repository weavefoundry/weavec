// Root-cause repro luaalloc: Lua's allocator shape, a free-or-realloc l_alloc reached only
// through the g->frealloc slot. v0.10.0 reports a double-free and a false use-after-free in
// tfree2 and misses the use-after-free in tfree.
// intended (RFC 0030 sections 9.1 and 9.3, gate S7): exactly two findings, both definite:
// the slot's known target l_alloc frees when nsize == 0, which mfree passes as a constant.
// EXPECT-LEDGER: /summary/errors == 2
// EXPECT-LEDGER: /summary/warnings == 0
// ASAN
#include <stdlib.h>
typedef void *(*Alloc)(void *ud, void *ptr, size_t osize, size_t nsize);
typedef struct G { Alloc frealloc; void *ud; } G;
typedef struct T { int flags; int *arr; } T;
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) { free(ptr); return NULL; }
    return realloc(ptr, nsize);
}
static void mfree(G *g, void *b, size_t osize) { (*g->frealloc)(g->ud, b, osize, 0); }
static void *mmalloc(G *g, size_t n) { void *p = (*g->frealloc)(g->ud, NULL, 0, n); if (!p) abort(); return p; }
G *newstate(Alloc f, void *ud) { G *g = f(ud, NULL, 0, sizeof(G)); if (!g) return NULL; g->frealloc = f; g->ud = ud; return g; }
void tfree(G *g, T *t) { mfree(g, t, sizeof(T)); t->flags = 0; } // BUG: use-after-free definite
void tfree2(G *g, T *t) { mfree(g, t->arr, 4); mfree(g, t->arr, 4); mfree(g, t, sizeof(T)); } // BUG: double-free definite
int main(void) { G *g = newstate(l_alloc, NULL); T *t; if (!g) return 1; t = mmalloc(g, sizeof(T)); t->arr = mmalloc(g, 4); tfree2(g, t); return 0; }
