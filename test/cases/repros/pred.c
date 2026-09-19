// Root-cause repro pred: a guard function that returns 0 for NULL; v0.10.0 reports
// 'dereference of it, which may be null' after 'if (!is_string(it)) return 0;' (false).
// intended (RFC 0030 section 9.2, gate S4): is_string's nonnull class proves 'it', no finding.
// CLEAN
// ASAN
#include <stdlib.h>
typedef struct item { int type; char *s; } item;
int is_string(const item *const it) { if (it == NULL) return 0; return (it->type & 0xFF) == 4; }
item *lookup(item **tab, int k) { return k > 0 ? tab[k] : NULL; }
char first(item **tab, int k) {
    item *it = lookup(tab, k);
    if (!is_string(it)) return 0;
    return it->type ? 'a' : 'b';
}
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) {
    item x = {4, "s"};
    item *tab[2] = {NULL, &x};
    return first(tab, 1) == 'a' && first(tab, 0) == 0 ? 0 : 1;
}
