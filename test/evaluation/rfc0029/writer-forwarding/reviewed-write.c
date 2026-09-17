#include <stdlib.h>
#include <string.h>
struct writer { unsigned char *data; size_t size, used, depth; int format; };
static unsigned char *reserve(struct writer *w, size_t needed) {
    if (!w || !w->data) return 0;
    if (w->size > 0 && w->used >= w->size) return 0;
    if (needed > 2147483647u) return 0;
    needed += w->used + 1;
    if (needed <= w->size) return w->data + w->used;
    return 0;
}
static int render(struct writer *w) {
  if (w->format) return render(w);
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
int forward(struct writer *w) { return render(w); }
int main(void) {
    struct writer w = {0};
    w.data = malloc(256);
    if (!w.data) return 0;
    w.size = 256;
    int result = forward(&w);
    if (result) (void)strlen((char *)w.data);
    free(w.data);
    return 0;
}
