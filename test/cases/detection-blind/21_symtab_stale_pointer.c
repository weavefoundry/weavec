// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Symbol table stored in a growable array, with references that point
 * directly at table entries. Interning a new symbol may realloc() the array,
 * which leaves the earlier references pointing into the freed old block;
 * the use count is then bumped through them.
 * Category: temporal (heap use-after-free via pointers into a reallocated
 * container).
 * Why it may be missed: the pointers are taken and used in different loops,
 * and the realloc() is inside intern(), out of sight of both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct symbol {
    char name[16];
    int value;
    unsigned uses;
};

struct symtab {
    struct symbol *syms;
    size_t len;
    size_t cap;
};

/* Returns the index of name, adding it if needed; (size_t)-1 on failure. */
static size_t intern(struct symtab *t, const char *name)
{
    for (size_t i = 0; i < t->len; i++)
        if (strcmp(t->syms[i].name, name) == 0)
            return i;
    if (t->len == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 2;
        struct symbol *p = realloc(t->syms, ncap * sizeof *p);
        if (!p)
            return (size_t)-1;
        t->syms = p;
        t->cap = ncap;
    }
    struct symbol *s = &t->syms[t->len];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->value = 0;
    s->uses = 0;
    return t->len++;
}

struct ref {
#ifdef FIX
    size_t sym; /* index: stays valid when the table grows */
#else
    struct symbol *sym; /* direct pointer for fast access */
#endif
};

int main(void)
{
    /* Statements "dst = src", as name pairs. */
    static const char *const stmts[][2] = {
        {"a", "b"}, {"c", "a"}, {"d", "c"}, {"e", "b"}, {"f", "e"},
    };
    struct symtab t = {NULL, 0, 0};
    struct ref refs[10];
    size_t nrefs = 0;
    for (size_t i = 0; i < 5; i++) {
        for (size_t k = 0; k < 2; k++) {
            size_t idx = intern(&t, stmts[i][k]);
            if (idx == (size_t)-1) {
                free(t.syms);
                return 1;
            }
#ifdef FIX
            refs[nrefs++].sym = idx;
#else
            refs[nrefs++].sym = &t.syms[idx];
#endif
        }
    }
    for (size_t k = 0; k < nrefs; k++)
#ifdef FIX
        t.syms[refs[k].sym].uses++;
#else
        refs[k].sym->uses++; // STOP
#endif
    unsigned total = 0;
    for (size_t i = 0; i < t.len; i++) {
        printf("%s: %u uses\n", t.syms[i].name, t.syms[i].uses);
        total += t.syms[i].uses;
    }
    free(t.syms);
    return total == nrefs && t.len == 6 ? 0 : 1;
}
