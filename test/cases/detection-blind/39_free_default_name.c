// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * display_name() returns a heap string for users with a name, but falls back
 * to a static default buffer for users without one. Callers free() the
 * result, which for the fallback is a global array.
 * Category: release (free of a non-heap pointer, a global).
 * Why it may be missed: the function's contract is "caller frees", which
 * holds on every path but the rare fallback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct user {
    const char *first;
    const char *last;
};

static char default_name[] = "anonymous";

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Returns the user's display name; the caller frees it. */
static char *display_name(const struct user *u)
{
    if (u->first && u->last) {
        size_t n = strlen(u->first) + strlen(u->last) + 2;
        char *s = malloc(n);
        if (s)
            snprintf(s, n, "%s %s", u->first, u->last);
        return s;
    }
    if (u->first)
        return dupstr(u->first);
#ifdef FIX
    return dupstr(default_name);
#else
    return default_name;
#endif
}

int main(void)
{
    static const struct user users[] = {{"Grace", "Hopper"}, {"Linus", NULL}, {NULL, NULL}};
    size_t total = 0;
    for (size_t i = 0; i < 3; i++) {
        char *name = display_name(&users[i]);
        if (!name)
            return 1;
        puts(name);
        total += strlen(name);
        free(name); // STOP
    }
    return total == 26 ? 0 : 1;
}
