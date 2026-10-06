// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * HTTP header parser. The value is duplicated and then trimmed with trim(),
 * which returns a pointer past any leading blanks; the struct keeps only the
 * trimmed pointer, and header_free() later passes that interior pointer to
 * free() for a value that had a leading space.
 * Category: release (free of an interior pointer).
 * Why it may be missed: dupstr() and trim() are composed on one line, and
 * values without leading blanks free the right pointer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Removes leading and trailing blanks. */
static char *trim(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t')
        start++;
    size_t n = strlen(start);
    while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t'))
        n--;
    start[n] = '\0';
#ifdef FIX
    memmove(s, start, n + 1);
    return s;
#else
    return start;
#endif
}

struct header {
    char *name;
    char *value;
};

static int header_parse(struct header *h, const char *line)
{
    const char *colon = strchr(line, ':');
    if (!colon)
        return -1;
    size_t n = (size_t)(colon - line);
    h->name = malloc(n + 1);
    if (!h->name)
        return -1;
    memcpy(h->name, line, n);
    h->name[n] = '\0';
    char *v = dupstr(colon + 1);
    if (!v) {
        free(h->name);
        return -1;
    }
    h->value = trim(v);
    return 0;
}

static void header_free(struct header *h)
{
    free(h->name);
    free(h->value); // STOP
}

int main(void)
{
    static const char *const lines[] = {"Host:example.org", "Content-Type: text/html ",
                                        "Accept:\t*/*"};
    size_t chars = 0;
    for (size_t i = 0; i < 3; i++) {
        struct header h;
        if (header_parse(&h, lines[i]) != 0)
            return 1;
        printf("[%s] = [%s]\n", h.name, h.value);
        chars += strlen(h.value);
        header_free(&h);
    }
    return chars == 23 ? 0 : 1;
}
