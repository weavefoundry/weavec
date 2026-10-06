// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Base64 encoder. The output buffer is sized with the textbook formula
 * (n + 2) / 3 * 4, which counts the encoded characters but not the NUL that
 * the encoder appends, so the terminator is written one byte past the block.
 * Category: spatial (heap buffer overflow by 1 byte).
 * Why it may be missed: the size formula is the familiar, correct one for
 * the encoded length; the missing +1 is easy to read past.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *b64_encode(const unsigned char *in, size_t n)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = (n + 2) / 3 * 4;
#ifdef FIX
    char *out = malloc(olen + 1);
#else
    char *out = malloc(olen);
#endif
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = '\0'; // STOP
    return out;
}

int main(void)
{
    static const struct {
        const char *plain;
        const char *encoded;
    } cases[] = {
        {"hello, world", "aGVsbG8sIHdvcmxk"},
        {"ab", "YWI="},
        {"user:secret", "dXNlcjpzZWNyZXQ="},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *p = cases[i].plain;
        char *e = b64_encode((const unsigned char *)p, strlen(p));
        if (!e)
            return 1;
        printf("%s -> %s\n", p, e);
        if (strcmp(e, cases[i].encoded) != 0)
            ok = 0;
        free(e);
    }
    return ok ? 0 : 1;
}
