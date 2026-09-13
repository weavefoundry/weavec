#include "runtime.h"
int reserve(struct buffer *b, size_t wanted) {
    if (wanted <= b->capacity) return 0;
    if (wanted > SIZE_MAX / sizeof(unsigned char)) return -1;
    unsigned char *p = realloc(b->data, wanted * sizeof(unsigned char));
    if (!p) return -1;
    b->data = p;
    b->capacity = wanted;
    return 0;
}
int append_impl(struct buffer *b, unsigned char value) {
    if (b->length == b->capacity) {
        if (b->capacity > SIZE_MAX - 16) return -1;
        if (reserve(b, b->capacity + 16)) return -1;
    }
    b->data[b->length] = value;
    ++b->length;
    return 0;
}
void truncate_buffer(struct buffer *b, size_t count) {
    if (count < b->length) b->length = count;
}
unsigned char *steal(struct buffer *b) {
    unsigned char *result = b->data;
    b->data = 0;
    b->length = 0;
    b->capacity = 0;
    return result;
}
void destroy(struct buffer *b) {
    free(b->data);
    b->data = 0;
    b->length = b->capacity = 0;
}
