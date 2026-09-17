#include <stdlib.h>
#include <string.h>
struct writer { unsigned char *data; size_t size, used, depth; int format; };
static unsigned char *reserve(struct writer *w, size_t needed) {
    if (!w || !w->data) return 0;
    if (w->size > 0 && w->used >= w->size) return 0;
    if (needed > 2147483647u) return 0;
    needed += w->used + 1;
    return needed <= w->size ? w->data + w->used : 0;
}
static int render(struct writer *w) {
    unsigned char *out = reserve(w, 2);
    if (!out) return 0;
    *out++ = '{';
    ++w->depth;
    ++w->used;
    out = reserve(w, 2);
    if (!out) return 0;
    *out++ = '}';
    *out = 0;
    --w->depth;
    return 1;
}
int main(void) {
    struct writer w = {0};
    w.data = malloc(256);
    if (!w.data) return 0;
    w.size = 256;
    int result = render(&w);
    if (result) (void)strlen((char *)w.data);
    free(w.data);
    return 0;
}
