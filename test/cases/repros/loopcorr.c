// Root-cause repro loopcorr: 'p' is non-null whenever i > 0, a correlation the loop does not
// keep; v0.10.0 reports 'dereference of p, which may be null' (false) and a leak of 'a' when
// malloc fails inside the loop (true, but only on out-of-memory).
// intended (RFC 0030 section 3.2): the maybe-null dereference is a checked facet, with no
// diagnostic and no trap; the out-of-memory leak may stay a warning, hence the ALLOW.
// CLEAN
// ALLOW: leak
// ASAN
#include <stdlib.h>
typedef struct node { struct node *next, *prev; } node;
node *chain(int count) {
    node *a = NULL, *p = NULL, *n = NULL; int i;
    a = malloc(sizeof *a); if (!a) return NULL; a->next = NULL;
    for (i = 0; a && i < count; i++) {
        n = malloc(sizeof *n);
        if (!n) { return NULL; }
        if (!i) a->next = n; else { p->next = n; n->prev = p; }
        p = n;
    }
    return a;
}
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) {
    node *a = chain(2);
    if (a) { node *n1 = a->next, *n2 = n1->next; free(n2); free(n1); free(a); }
    return 0;
}
