// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Report builder whose append_line() works like realloc(): it returns the
 * possibly moved buffer. Every call site assigns the result except the one
 * that appends the footer; that append grows the buffer, the caller keeps
 * the old pointer, and the final strlen() reads the freed block.
 * Category: temporal (heap use after realloc moved the block).
 * Why it may be missed: the footer line looks like all the others, and it
 * only moves the buffer when the report happens to be near its capacity.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Appends text and a newline; returns the (possibly moved) buffer, or NULL
   after freeing it on allocation failure. */
static char *append_line(char *buf, size_t *len, size_t *cap, const char *text)
{
    size_t n = strlen(text);
    if (*len + n + 2 > *cap) {
        size_t ncap = *cap * 2 + n + 2;
        char *p = realloc(buf, ncap);
        if (!p) {
            free(buf);
            return NULL;
        }
        buf = p;
        *cap = ncap;
    }
    memcpy(buf + *len, text, n);
    buf[*len + n] = '\n';
    buf[*len + n + 1] = '\0';
    *len += n + 1;
    return buf;
}

int main(void)
{
    static const struct {
        const char *name;
        unsigned mb;
    } vols[] = {{"root", 1024}, {"home", 210}};
    size_t len = 0, cap = 64;
    char *report = malloc(cap);
    if (!report)
        return 1;
    report[0] = '\0';
    report = append_line(report, &len, &cap, "== disk usage ==");
    unsigned total = 0;
    for (size_t i = 0; report && i < 2; i++) {
        char line[32];
        snprintf(line, sizeof line, "%-8s %5u MB", vols[i].name, vols[i].mb);
        report = append_line(report, &len, &cap, line);
        total += vols[i].mb;
    }
    if (!report)
        return 1;
    char footer[48];
    snprintf(footer, sizeof footer, "total: %u MB in %zu volumes", total, sizeof vols / sizeof vols[0]);
#ifdef FIX
    report = append_line(report, &len, &cap, footer);
#else
    append_line(report, &len, &cap, footer);
#endif
    if (!report)
        return 1;
    size_t shown = strlen(report); // STOP
    fputs(report, stdout);
    free(report);
    return shown == len ? 0 : 1;
}
