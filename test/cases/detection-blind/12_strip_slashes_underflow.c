// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Path normaliser that strips trailing slashes by walking an end pointer
 * backwards. For the root path "/" the loop removes the only character and
 * then inspects end[-1], the byte before the start of the stack buffer.
 * Category: spatial (stack buffer underflow, read of path[-1]).
 * Why it may be missed: the loop is correct for every path but the root, and
 * the underflowing read happens in the loop condition.
 */
#include <stdio.h>
#include <string.h>

static void strip_trailing_slashes(char *path)
{
    char *end = path + strlen(path);
#ifdef FIX
    while (end > path + 1 && end[-1] == '/')
#else
    while (end[-1] == '/') // STOP
#endif
        *--end = '\0';
}

int main(void)
{
    static const char *const in[] = {"/var/log/", "/tmp//", "relative/dir/", "/"};
    static const char *const want[] = {"/var/log", "/tmp", "relative/dir", "/"};
    int ok = 1;
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", in[i]);
        strip_trailing_slashes(buf);
        printf("%s -> %s\n", in[i], buf);
        if (strcmp(buf, want[i]) != 0)
            ok = 0;
    }
    return ok ? 0 : 1;
}
