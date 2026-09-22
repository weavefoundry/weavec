// Root-cause repro arrloop: array cells freed in cleanup loops; v0.10.0 reports 'v[i]' freed
// twice and four leaks naming v[0] and v[array-index(i)] (all false).
// intended (RFC 0030 section 3.1): no error. A free that a later loop iteration may repeat is
// at most a possible finding, which is a warning, and a leak is never an error; so
// double-free and leak warnings are allowed, and the program must not trap.
// CLEAN
// ALLOW: double-free leak
// ASAN
#include <stdlib.h>
void free_all(char **v, int n) { int i; for (i = 0; i < n; i++) free(v[i]); free(v); }
char **make(int n) {
    int i; char **v = malloc(sizeof(char*) * (size_t)n);
    if (!v) return NULL;
    for (i = 0; i < n; i++) { v[i] = malloc(4); if (!v[i]) { while (i--) free(v[i]); free(v); return NULL; } }
    return v;
}
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) {
    char **v = make(4);
    if (!v) return 1;
    free_all(v, 4);
    return 0;
}
