// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Lexer over a source buffer that, like a file read with fread(), has an
 * explicit length and no terminator. peek_is() looks at the next character
 * for two-character operators without checking the length, so a source that
 * ends in '=' or '<' reads one byte past the heap buffer.
 * Category: spatial (heap buffer over-read by 1 byte).
 * Why it may be missed: the code was written for NUL-terminated strings,
 * where src[len] is always readable; only the buffer's origin changed.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lexer {
    const char *src;
    size_t len;
    size_t pos;
};

enum tok { T_EOF, T_IDENT, T_NUM, T_ASSIGN, T_EQ, T_LT, T_LE, T_SEMI, T_ERR };

static int peek_is(const struct lexer *lx, char want)
{
#ifdef FIX
    if (lx->pos >= lx->len)
        return 0;
#endif
    return lx->src[lx->pos] == want; // STOP
}

static int is_ident(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static enum tok next_token(struct lexer *lx)
{
    while (lx->pos < lx->len && isspace((unsigned char)lx->src[lx->pos]))
        lx->pos++;
    if (lx->pos >= lx->len)
        return T_EOF;
    char c = lx->src[lx->pos++];
    if (isalpha((unsigned char)c) || c == '_') {
        while (lx->pos < lx->len && is_ident(lx->src[lx->pos]))
            lx->pos++;
        return T_IDENT;
    }
    if (isdigit((unsigned char)c)) {
        while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos]))
            lx->pos++;
        return T_NUM;
    }
    switch (c) {
    case '=':
        if (peek_is(lx, '=')) {
            lx->pos++;
            return T_EQ;
        }
        return T_ASSIGN;
    case '<':
        if (peek_is(lx, '=')) {
            lx->pos++;
            return T_LE;
        }
        return T_LT;
    case ';':
        return T_SEMI;
    default:
        return T_ERR;
    }
}

int main(void)
{
    static const char text[] = "x = 1; y = x <= 2; done =";
    size_t len = strlen(text);
    char *src = malloc(len);
    if (!src)
        return 1;
    memcpy(src, text, len); /* as if read from a file: no terminator */
    struct lexer lx = {src, len, 0};
    unsigned counts[T_ERR + 1] = {0};
    unsigned ntok = 0;
    enum tok t;
    while ((t = next_token(&lx)) != T_EOF) {
        counts[t]++;
        ntok++;
    }
    free(src);
    printf("%u tokens, %u comparisons\n", ntok, counts[T_LE] + counts[T_EQ]);
    return ntok == 12 && counts[T_ERR] == 0 ? 0 : 1;
}
