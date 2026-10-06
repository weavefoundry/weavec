// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Command dispatcher. build_table() fills a heap table with one entry per
 * command, but lookup() walks the table until it finds an entry whose name
 * is NULL, and no such sentinel was allocated. Looking up an unknown command
 * reads past the end of the table.
 * Category: spatial (heap buffer over-read, missing sentinel).
 * Why it may be missed: every lookup of a known command stops early; only a
 * miss walks to the (absent) terminator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*cmd_fn)(int);

struct command {
    const char *name;
    cmd_fn run;
};

static int cmd_double(int x) { return 2 * x; }
static int cmd_negate(int x) { return -x; }
static int cmd_square(int x) { return x * x; }

/* Builds the dispatch table; lookups stop at the entry whose name is NULL. */
static struct command *build_table(const char *const *names, const cmd_fn *fns, size_t n)
{
#ifdef FIX
    struct command *t = malloc((n + 1) * sizeof *t);
#else
    struct command *t = malloc(n * sizeof *t);
#endif
    if (!t)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        t[i].name = names[i];
        t[i].run = fns[i];
    }
#ifdef FIX
    t[n].name = NULL;
    t[n].run = NULL;
#endif
    return t;
}

static const struct command *lookup(const struct command *t, const char *name)
{
    for (const struct command *c = t; c->name != NULL; c++) // STOP
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

int main(void)
{
    static const char *const names[] = {"double", "negate", "square"};
    static const cmd_fn fns[] = {cmd_double, cmd_negate, cmd_square};
    static const char *const script[] = {"double", "square", "half", "negate"};
    struct command *table = build_table(names, fns, 3);
    if (!table)
        return 1;
    int x = 3;
    unsigned unknown = 0;
    for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
        const struct command *c = lookup(table, script[i]);
        if (!c) {
            fprintf(stderr, "unknown command '%s'\n", script[i]);
            unknown++;
            continue;
        }
        x = c->run(x);
    }
    free(table);
    printf("result %d\n", x);
    return x == -36 && unknown == 1 ? 0 : 1;
}
