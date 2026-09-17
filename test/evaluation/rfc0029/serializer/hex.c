#include "hex.h"
#include <stdlib.h>
static int push(struct text *out, unsigned char value) {
    if (out->written == out->allocated) {
        if (out->allocated > 1048576) return 0;
        size_t next = out->allocated + 32;
        unsigned char *grown = realloc(out->storage, next);
        if (!grown) return 0;
        out->storage = grown;
        out->allocated = next;
    }
    out->storage[out->written++] = value;
    return 1;
}
int hex_encode(struct text *out, const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = data[i];
        unsigned char high = byte >> 4;
        unsigned char low = byte & 15;
        if (!push(out, high < 10 ? '0' + high : 'a' + high - 10)) return 0;
        if (!push(out, low < 10 ? '0' + low : 'a' + low - 10)) return 0;
    }
    return 1;
}
void hex_dispose(struct text *out) {
    free(out->storage);
    out->storage = 0;
    out->written = 0;
    out->allocated = 0;
}
