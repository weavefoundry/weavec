// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Parser that publishes its stack-allocated context in a global so that deep
 * helpers can prefix warnings with file and line. parse_buffer() returns
 * without clearing the global, and a later validation warning reads the
 * context of the dead frame.
 * Category: temporal (stack use after return, through a global pointer).
 * Why it may be missed: warn() checks the global for NULL, which looks like
 * proper lifetime handling; the dangling case is a non-NULL stale pointer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parse_ctx {
    const char *file;
    unsigned line;
};

static struct parse_ctx *g_ctx; /* set while a parse is running */
static unsigned g_warnings;

static void warn(const char *msg)
{
    g_warnings++;
    if (g_ctx)
        fprintf(stderr, "%s:%u: warning: %s\n", g_ctx->file, g_ctx->line, msg); // STOP // MISS: a stack object is untracked once its scope ends (RFC 0032 §4): use after return
    else
        fprintf(stderr, "warning: %s\n", msg);
}

struct totals {
    unsigned records;
    unsigned empty;
    long sum;
};

static void parse_buffer(const char *name, const char *text, struct totals *t)
{
    struct parse_ctx ctx = {name, 1};
    g_ctx = &ctx;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n == 0) {
            t->empty++;
            warn("empty record");
        } else {
            t->records++;
            t->sum += strtol(p, NULL, 10);
        }
        p += n + (nl ? 1 : 0);
        ctx.line++;
    }
#ifdef FIX
    g_ctx = NULL;
#endif
}

static void check_totals(const struct totals *t)
{
    if (t->empty > 0)
        warn("input contained empty records");
}

int main(void)
{
    struct totals t = {0, 0, 0};
    parse_buffer("data.txt", "10\n20\n\n30\n", &t);
    check_totals(&t);
    printf("%u records, sum %ld, %u warnings\n", t.records, t.sum, g_warnings);
    return t.records == 3 && t.sum == 60 && g_warnings == 2 ? 0 : 1;
}
