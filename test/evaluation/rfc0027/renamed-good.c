/* RFC 0027: frozen recursive ownership clients, with no trusted annotations. */
#include <stdlib.h>
struct branch { unsigned value; struct branch *first, *second; };
static unsigned visit(const struct branch *p) {
    if (!p) return 0;
    return p->value + visit(p->first) + visit(p->second);
}
static void destroy(struct branch *p) {
    if (!p) return;
    destroy(p->first);
    destroy(p->second);
    free(p);
}
static struct branch *make(unsigned n) {
    struct branch *root = NULL;
    for (unsigned i = 0; i < n; ++i) {
        struct branch *p = malloc(sizeof *p);
        if (!p) { destroy(root); return NULL; }
        p->value = i;
        p->first = root;
        p->second = NULL;
        root = p;
    }
    return root;
}
static struct branch *detach(struct branch *p) {
    struct branch *child = p->first;
    p->first = NULL;
    return child;
}
static void attach(struct branch *p, struct branch *child) { p->first = child; }

int main(void) {struct branch a={1,0,0},b={2,0,0},p={3,&a,&b};return visit(&p)!=6;}
