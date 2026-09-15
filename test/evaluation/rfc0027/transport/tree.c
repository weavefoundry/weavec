#include "tree-api.h"
unsigned total(const struct node *p) {
    if (!p) return 0;
    return p->value + total(p->left) + total(p->right);
}
void destroy(struct node *p) {
    if (!p) return;
    destroy(p->left);
    destroy(p->right);
    free(p);
}
struct node *make(unsigned n) {
    struct node *root = NULL;
    for (unsigned i = 0; i < n; ++i) {
        struct node *p = malloc(sizeof *p);
        if (!p) { destroy(root); return NULL; }
        p->value = i;
        p->left = root;
        p->right = NULL;
        root = p;
    }
    return root;
}
struct node *detach(struct node *p) {
    struct node *child = p->left;
    p->left = NULL;
    return child;
}
void attach(struct node *p, struct node *child) { p->left = child; }
