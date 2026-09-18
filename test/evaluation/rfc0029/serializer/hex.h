#ifndef RFC29_HEX_H
#define RFC29_HEX_H
#include <stddef.h>
struct text {
    unsigned generation;
    size_t allocated;
    unsigned char *storage;
    unsigned flags;
    size_t written;
    size_t depth;
};
int hex_encode(struct text *out, const unsigned char *data, size_t size);
void hex_dispose(struct text *out);
#endif
