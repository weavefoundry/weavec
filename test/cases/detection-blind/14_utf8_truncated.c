// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * UTF-8 decoder for network chunks. The sequence length comes from the lead
 * byte, and the continuation bytes are read without checking that the chunk
 * still holds them, so a chunk cut in the middle of a character reads past
 * the end of its exactly-sized heap buffer.
 * Category: spatial (heap buffer over-read, length computed from the data).
 * Why it may be missed: the decoder validates every continuation byte, which
 * looks thorough, and well-formed test strings never end mid-sequence.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t seq_len(unsigned char lead)
{
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 0;
}

/* Decodes s[0..n) and returns the number of code points, or -1. */
static long utf8_decode(const unsigned char *s, size_t n, uint32_t *out, size_t outcap)
{
    size_t i = 0, k = 0;
    while (i < n) {
        size_t len = seq_len(s[i]);
        if (len == 0)
            return -1;
#ifdef FIX
        if (len > n - i)
            return -1; /* truncated sequence */
#endif
        uint32_t cp = len == 1 ? s[i] : s[i] & (0x7Fu >> len);
        for (size_t j = 1; j < len; j++) {
            unsigned char c = s[i + j]; // STOP
            if ((c & 0xC0) != 0x80)
                return -1;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (k < outcap)
            out[k] = cp;
        k++;
        i += len;
    }
    return (long)k;
}

static unsigned char *load(const char *bytes, size_t n)
{
    unsigned char *b = malloc(n ? n : 1);
    if (b)
        memcpy(b, bytes, n);
    return b;
}

int main(void)
{
    /* The second chunk was cut in the middle of a 3-byte sequence. */
    static const char *const chunks[] = {
        "na\xc3\xafve caf\xc3\xa9",
        "price \xe2\x82\xac" "5 \xe6\x97",
    };
    static const long expected[] = {10, -1};
    uint32_t cps[32];
    int ok = 1;
    for (size_t i = 0; i < 2; i++) {
        size_t n = strlen(chunks[i]);
        unsigned char *buf = load(chunks[i], n);
        if (!buf)
            return 1;
        long k = utf8_decode(buf, n, cps, 32);
        printf("chunk %zu: %ld code points\n", i, k);
        if (k != expected[i])
            ok = 0;
        free(buf);
    }
    return ok ? 0 : 1;
}
