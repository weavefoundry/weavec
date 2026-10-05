// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * String builder: sb_reserve() makes room for `extra` characters but not for
 * the terminating NUL that sb_append() copies along with them. When the text
 * exactly fills the capacity the NUL lands one byte past the heap block.
 * Category: spatial (heap buffer overflow by 1 byte).
 * Why it may be missed: the off-by-one only shows when a string ends exactly
 * on a power-of-two boundary; most appends leave slack in the buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct strbuf {
    char *data;
    size_t len;
    size_t cap;
};

static int sb_reserve(struct strbuf *b, size_t extra)
{
    size_t need = b->len + extra;
#ifdef FIX
    need += 1; /* room for the terminator */
#endif
    if (need <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 16;
    while (ncap < need)
        ncap *= 2;
    char *p = realloc(b->data, ncap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = ncap;
    return 0;
}

static int sb_append(struct strbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (sb_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, s, n + 1); // STOP
    b->len += n;
    return 0;
}

int main(void)
{
    static const char *const parts[] = {"usr", "local", "share", "doc"};
    struct strbuf b = {NULL, 0, 0};
    int ok = 1;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        if (sb_append(&b, "/") != 0 || sb_append(&b, parts[i]) != 0) {
            ok = 0;
            break;
        }
    }
    if (ok) {
        printf("%s\n", b.data);
        ok = strcmp(b.data, "/usr/local/share/doc") == 0;
    }
    free(b.data);
    return ok ? 0 : 1;
}
