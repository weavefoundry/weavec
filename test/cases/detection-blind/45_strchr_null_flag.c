// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Key=value configuration reader. parse_line() splits each line at the '='
 * found by strchr(), without handling the case where there is none; a bare
 * flag line such as "verbose" makes it write through a NULL pointer.
 * Category: null (unchecked NULL from a library function).
 * Why it may be missed: blank and comment lines are filtered out before the
 * call, which makes the input look validated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kv {
    const char *key;
    const char *val;
};

static int parse_line(char *line, struct kv *out)
{
    char *eq = strchr(line, '=');
#ifdef FIX
    if (!eq) { /* bare flag */
        out->key = line;
        out->val = "true";
        return 0;
    }
#endif
    *eq = '\0'; // STOP
    out->key = line;
    out->val = eq + 1;
    return 0;
}

int main(void)
{
    static const char *const lines[] = {"# server settings", "name=demo", "port=8080",
                                        "", "verbose", "root=/srv"};
    enum { N = sizeof lines / sizeof lines[0] };
    char store[N][64];
    struct kv kvs[N];
    size_t n = 0;
    for (size_t i = 0; i < N; i++) {
        if (lines[i][0] == '\0' || lines[i][0] == '#')
            continue;
        snprintf(store[n], sizeof store[n], "%s", lines[i]);
        if (parse_line(store[n], &kvs[n]) == 0)
            n++;
    }
    long port = 0;
    for (size_t i = 0; i < n; i++) {
        printf("%s -> %s\n", kvs[i].key, kvs[i].val);
        if (strcmp(kvs[i].key, "port") == 0)
            port = strtol(kvs[i].val, NULL, 10);
    }
    return n == 4 && port == 8080 ? 0 : 1;
}
