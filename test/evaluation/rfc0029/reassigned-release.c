#include <stdlib.h>
static void destroy(void *p) { free(p); p = 0; }
int main(void) {
    void *p = malloc(8);
    if (!p) return 0;
    destroy(p);
    return 0;
}
