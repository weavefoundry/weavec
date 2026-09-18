/* RFC 0029: frozen field-role and helper-composition population. */
#include <stdint.h>
#include <stdlib.h>
struct output {
    unsigned char *bytes;
    size_t depth;
    size_t used;
    unsigned mode;
    size_t available;
    size_t generation;
};
static int reserve(struct output *b, size_t wanted) {
    if (wanted <= b->available) return 0;
    unsigned char *p = realloc(b->bytes, wanted);
    if (!p) return -1;
    b->bytes = p;
    b->available = wanted;
    return 0;
}
static int append(struct output *b, unsigned char value) {
    if (b->used == b->available) {
        if (b->available > SIZE_MAX - 8) return -1;
        if (reserve(b, b->available + 8)) return -1;
    }
    b->bytes[b->used] = value;
    ++b->used;
    return 0;
}
static unsigned char last(struct output *b) {
    return b->used ? b->bytes[b->used - 1] : 0;
}
