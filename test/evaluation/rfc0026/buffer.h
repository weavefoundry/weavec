/* RFC 0026: frozen runtime buffer operations; no trusted annotations. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef ELEMENT
#define ELEMENT unsigned char
#endif
struct buffer { ELEMENT *data; size_t length, capacity; };
static int reserve(struct buffer *b, size_t wanted) {
    if (wanted <= b->capacity) return 0;
    if (wanted > SIZE_MAX / sizeof(ELEMENT)) return -1;
    ELEMENT *p = realloc(b->data, wanted * sizeof(ELEMENT));
    if (!p) return -1;
    b->data = p;
    b->capacity = wanted;
    return 0;
}
static int append(struct buffer *b, ELEMENT value) {
    if (b->length == b->capacity) {
        if (b->capacity > SIZE_MAX - 16) return -1;
        if (reserve(b, b->capacity + 16)) return -1;
    }
    b->data[b->length] = value;
    ++b->length;
    return 0;
}
static void truncate_buffer(struct buffer *b, size_t count) {
    if (count < b->length) b->length = count;
}
static ELEMENT *steal(struct buffer *b) {
    ELEMENT *result = b->data;
    b->data = 0;
    b->length = 0;
    b->capacity = 0;
    return result;
}
static void destroy(struct buffer *b) {
    free(b->data);
    b->data = 0;
    b->length = b->capacity = 0;
}
