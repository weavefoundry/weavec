// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Formats user names for an 8-column report. strncpy() into an 8-byte stack
 * buffer does not terminate the copy when the login has 8 or more
 * characters, and the strlen() that follows runs off the end of the buffer.
 * Category: spatial (stack buffer over-read via strlen of an unterminated
 * buffer).
 * Why it may be missed: strncpy() with sizeof looks like the "safe" idiom;
 * short logins (the common case) are terminated by its zero padding.
 */
#include <stdio.h>
#include <string.h>

/* Formats "name(uid)" with the login cut to fit an 8-character column and
   returns the width of the name part. */
static size_t format_user(const char *login, unsigned uid, char *out, size_t outsz)
{
    char shortname[8];
    strncpy(shortname, login, sizeof shortname);
#ifdef FIX
    shortname[sizeof shortname - 1] = '\0';
#endif
    size_t n = strlen(shortname); // STOP
    snprintf(out, outsz, "%s(%u)", shortname, uid);
    return n;
}

int main(void)
{
    static const struct {
        const char *login;
        unsigned uid;
    } users[] = {
        {"root", 0},
        {"daemon", 1},
        {"postmaster", 25},
        {"www", 33},
    };
    char line[32];
    size_t width = 0;
    for (size_t i = 0; i < sizeof users / sizeof users[0]; i++) {
        size_t n = format_user(users[i].login, users[i].uid, line, sizeof line);
        if (n > width)
            width = n;
        puts(line);
    }
    return width <= 7 ? 0 : 1;
}
