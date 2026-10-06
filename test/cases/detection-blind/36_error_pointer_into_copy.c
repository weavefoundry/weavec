// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Parser of comma-separated "key=value" pairs that works on a private copy
 * of its input (strtok() modifies it). On a syntax error it reports the
 * offending token through an out-parameter that points into the copy, and
 * then frees the copy; the caller prints the token from freed memory.
 * Category: temporal (heap use-after-free across a function boundary, on an
 * error path).
 * Why it may be missed: error paths are rarely tested, and *err looks like
 * it points at the caller's own string.
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

/* Sums the values of the pairs; returns the number of pairs, or -1 with *err
   pointing at the offending text. */
static int sum_pairs(const char *text, long *sum, const char **err)
{
    char *copy = dupstr(text);
    int n = 0;
    *err = text;
    if (!copy)
        return -1;
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) {
#ifdef FIX
            *err = text + (tok - copy);
#else
            *err = tok;
#endif
            n = -1;
            break;
        }
        *sum += strtol(eq + 1, NULL, 10);
        n++;
    }
    free(copy);
    return n;
}

int main(void)
{
    static const char *const inputs[] = {"a=1,b=2,c=3", "width=10,height,depth=4"};
    long total = 0;
    unsigned errors = 0;
    for (size_t i = 0; i < 2; i++) {
        long sum = 0;
        const char *err;
        if (sum_pairs(inputs[i], &sum, &err) < 0) {
            fprintf(stderr, "syntax error near '%.6s'\n", err); // STOP
            errors++;
        } else {
            total += sum;
        }
    }
    printf("total %ld, %u errors\n", total, errors);
    return total == 6 && errors == 1 ? 0 : 1;
}
