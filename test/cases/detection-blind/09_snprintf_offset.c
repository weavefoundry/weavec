// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Renders "key=value" pairs into a 48-byte stack line with successive
 * snprintf() calls at an advancing offset. Each call passes the size of the
 * whole buffer instead of what is left after the offset, so the pair that
 * crosses the end is written past the buffer instead of being truncated.
 * Category: spatial (stack buffer overflow, wrong length to snprintf).
 * Why it may be missed: snprintf() with sizeof reads as bounded; the size is
 * correct for the first call and only wrong once off > 0.
 */
#include <stdio.h>
#include <string.h>

struct kv {
    const char *key;
    const char *val;
};

/* Prints the pairs on one line, truncated to the line width, and returns the
   number of characters printed. */
static size_t render(const struct kv *kvs, size_t n)
{
    char line[48];
    size_t off = 0;
    line[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        const char *sep = i ? " " : "";
#ifdef FIX
        if (off >= sizeof line)
            break;
        int w = snprintf(line + off, sizeof line - off, "%s%s=%s", sep, kvs[i].key, kvs[i].val);
#else
        int w = snprintf(line + off, sizeof line, "%s%s=%s", sep, kvs[i].key, kvs[i].val); // STOP
#endif
        if (w < 0)
            return 0;
        off += (size_t)w;
    }
    printf("%s\n", line);
    return strlen(line);
}

int main(void)
{
    static const struct kv conf[] = {
        {"host", "example.org"},
        {"port", "8080"},
        {"user", "deploy"},
        {"path", "/srv/app/releases/current"},
    };
    size_t shown = render(conf, sizeof conf / sizeof conf[0]);
    return shown == 47 ? 0 : 1;
}
