#include "api.h"
#include <stdlib.h>
int main(void) {
    struct node *a = calloc(1, sizeof *a);
    if (!a) return 0;
    struct node *b = calloc(1, sizeof *b);
    if (!b) { free(a); return 0; }
    a->right = b;
    destroy_even(a);
    void *bytes = make(malloc, 19);
    dispose(free, bytes);
    return 0;
}
