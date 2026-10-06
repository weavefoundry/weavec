// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
// (The second STOP, at the call, is where the null reaches a callee that
// dereferences it unconditionally: a stop there is before the access.)
/*
 * Shape list parser. shape_parse() returns NULL for a shape kind it does not
 * know (an expected error path, not an allocation failure), and the caller
 * passes the result straight to area(), which dereferences it.
 * Category: null (NULL returned on an error path, dereferenced by a callee).
 * Why it may be missed: NULL returns look like out-of-memory handling that
 * "never happens"; the unknown-kind path is easy to forget.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum kind { CIRCLE, RECT };

struct shape {
    enum kind kind;
    double a, b;
};

static struct shape *shape_parse(const char *spec)
{
    char kind[16];
    double a = 0, b = 0;
    int n = sscanf(spec, "%15s %lf %lf", kind, &a, &b);
    if (n < 2)
        return NULL;
    struct shape *s = malloc(sizeof *s);
    if (!s)
        return NULL;
    if (strcmp(kind, "circle") == 0) {
        s->kind = CIRCLE;
    } else if (strcmp(kind, "rect") == 0 && n == 3) {
        s->kind = RECT;
    } else {
        free(s); /* unknown kind */
        return NULL;
    }
    s->a = a;
    s->b = b;
    return s;
}

static double area(const struct shape *s)
{
    switch (s->kind) { // STOP
    case CIRCLE:
        return 3.25 * s->a * s->a;
    case RECT:
        return s->a * s->b;
    }
    return 0;
}

int main(void)
{
    static const char *const specs[] = {"circle 2", "rect 2 3", "triangle 3 4", "rect 1 1"};
    double total = 0;
    unsigned shapes = 0;
    for (size_t i = 0; i < 4; i++) {
        struct shape *s = shape_parse(specs[i]);
#ifdef FIX
        if (!s) {
            fprintf(stderr, "skipping '%s'\n", specs[i]);
            continue;
        }
#endif
        total += area(s); // STOP
        shapes++;
        free(s);
    }
    printf("%u shapes, total area %.2f\n", shapes, total);
    return shapes == 3 && total == 20.0 ? 0 : 1;
}
