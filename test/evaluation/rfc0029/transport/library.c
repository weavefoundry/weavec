#include "api.h"
#include <stdlib.h>
void destroy_even(struct node *p) {
    if (!p) return;
    destroy_odd(p->left); destroy_odd(p->right); free(p);
}
void destroy_odd(struct node *p) {
    if (!p) return;
    destroy_even(p->left); destroy_even(p->right); free(p);
}
void *make(void *(*allocate)(size_t), size_t count) {
    if (!count) return 0;
    unsigned char *p = allocate(count);
    if (p) p[count - 1] = 7;
    return p;
}
void dispose(void (*release)(void *), void *p) { release(p); p = 0; }
