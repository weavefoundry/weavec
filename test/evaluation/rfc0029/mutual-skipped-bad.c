#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
static void destroy_even(struct node *p);
static void destroy_odd(struct node *p) {
    if (!p) return;
    destroy_even(p->left);
    free(p);
}
static void destroy_even(struct node *p) {
    if (!p) return;
    destroy_odd(p->left);
    destroy_odd(p->right);
    free(p);
}
int main(void) {
    struct node *a = calloc(1, sizeof *a);
    if (!a) return 0;
    struct node *b = calloc(1, sizeof *b);
    if (!b) { free(a); return 0; }
    struct node *c = calloc(1, sizeof *c);
    if (!c) { free(a); free(b); return 0; }
    a->left = b;
    b->right = c;
    destroy_even(a);
    return 0;
}
