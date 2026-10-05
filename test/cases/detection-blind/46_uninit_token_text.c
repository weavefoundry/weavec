// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Tiny calculator that echoes its tokens. lex() fills in text and len for
 * words, but for numbers it sets only num and len; the echo then copies
 * tok.text, which for a leading number was never initialised.
 * Category: uninitialized (read through an uninitialised pointer field of a
 * stack struct).
 * Why it may be missed: compilers do not track struct fields filled through
 * a pointer, and after the first word token the field holds a stale but
 * valid pointer, so only a leading number shows the bug.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum kind { TK_END, TK_NUM, TK_WORD };

struct token {
    enum kind kind;
    long num;
    const char *text;
    size_t len;
};

static const char *lex(const char *p, struct token *t)
{
    while (*p == ' ')
        p++;
    if (*p == '\0') {
        t->kind = TK_END;
        t->len = 0;
        return p;
    }
    if (isdigit((unsigned char)*p)) {
        char *end;
        t->kind = TK_NUM;
        t->num = strtol(p, &end, 10);
#ifdef FIX
        t->text = p;
#endif
        t->len = (size_t)(end - p);
        return end;
    }
    size_t n = 0;
    while (p[n] != '\0' && p[n] != ' ')
        n++;
    t->kind = TK_WORD;
    t->text = p;
    t->len = n;
    return p + n;
}

int main(void)
{
    const char *src = "12 plus 30 minus 2";
    char echo[64];
    size_t eo = 0;
    long acc = 0;
    int sign = 1;
    struct token tok;
    const char *p = src;
    for (;;) {
        p = lex(p, &tok);
        if (tok.kind == TK_END)
            break;
        if (eo + tok.len + 1 < sizeof echo) {
            memcpy(echo + eo, tok.text, tok.len); // STOP // MISS: zero-initialisation makes the never-set pointer null; memcpy's null check is inexpressible (the token is a local whose address is taken)
            eo += tok.len;
            echo[eo++] = ' ';
        }
        if (tok.kind == TK_NUM)
            acc += sign * tok.num;
        else
            sign = tok.len == 5 && strncmp(tok.text, "minus", 5) == 0 ? -1 : 1;
    }
    echo[eo ? eo - 1 : 0] = '\0';
    printf("%s = %ld\n", echo, acc);
    return acc == 40 && strcmp(echo, src) == 0 ? 0 : 1;
}
