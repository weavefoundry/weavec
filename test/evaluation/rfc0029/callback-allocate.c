#include <stdlib.h>
static void *create(void *(*allocate)(size_t), size_t n) {
    if (!n) return 0;
    unsigned char *p = allocate(n);
    if (p) p[n - 1] = 7;
    return p;
}
int main(void) {
    void *p = create(malloc, 8);
    free(p);
    return 0;
}
