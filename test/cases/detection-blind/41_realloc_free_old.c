// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Growable byte blob. blob_reserve() assumes that when realloc() moves the
 * block it leaves the old one for the caller to release, and frees the old
 * pointer itself; realloc() has already freed it, so this is a double free
 * whenever the block moves.
 * Category: release (double free after realloc).
 * Why it may be missed: the first reserve passes NULL (free(NULL) is fine),
 * and small growths are often done in place, so the free is rarely reached.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct blob {
    unsigned char *data;
    size_t len;
    size_t cap;
};

static int blob_reserve(struct blob *b, size_t need)
{
    if (need <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need)
        ncap *= 2;
    unsigned char *p = realloc(b->data, ncap);
    if (!p)
        return -1;
#ifndef FIX
    if (p != b->data)
        free(b->data); // STOP
#endif
    b->data = p;
    b->cap = ncap;
    return 0;
}

static int blob_append(struct blob *b, const void *src, size_t n)
{
    if (blob_reserve(b, b->len + n) != 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

int main(void)
{
    struct blob b = {NULL, 0, 0};
    static const unsigned char header[16] = "BLOB\x01\x00\x00\x00";
    unsigned char chunk[1500];
    for (size_t i = 0; i < sizeof chunk; i++)
        chunk[i] = (unsigned char)(i * 7u);
    if (blob_append(&b, header, sizeof header) != 0 || blob_append(&b, chunk, sizeof chunk) != 0) {
        free(b.data);
        return 1;
    }
    unsigned sum = 0;
    for (size_t i = 0; i < b.len; i++)
        sum += b.data[i];
    printf("%zu bytes in a %zu-byte blob, sum %u\n", b.len, b.cap, sum);
    free(b.data);
    return b.len == 1516 && b.cap == 2048 ? 0 : 1;
}
