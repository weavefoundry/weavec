// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Wire message decoder. When the body fails validation, msg_decode() frees
 * the body it allocated but leaves m->body pointing at it; the caller then
 * cleans up with msg_clear(), as it does for every message, and frees the
 * body a second time.
 * Category: release (double free across functions, on an error path).
 * Why it may be missed: both functions "clean up after themselves", and the
 * failing path is taken only for a body with a non-printable character.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct msg {
    int type;
    size_t len;
    char *body;
};

/* Decodes "<type>:<len>:<body>". */
static int msg_decode(struct msg *m, const char *wire)
{
    char *end;
    long type = strtol(wire, &end, 10);
    if (*end != ':' || type < 0 || type > 255)
        return -1;
    unsigned long len = strtoul(end + 1, &end, 10);
    if (*end != ':')
        return -1;
    const char *body = end + 1;
    if (strlen(body) != len)
        return -1;
    m->type = (int)type;
    m->len = len;
    m->body = malloc(len + 1);
    if (!m->body)
        return -1;
    memcpy(m->body, body, len + 1);
    for (size_t i = 0; i < len; i++) {
        if (!isprint((unsigned char)m->body[i])) {
            free(m->body);
#ifdef FIX
            m->body = NULL;
#endif
            return -1;
        }
    }
    return 0;
}

static void msg_clear(struct msg *m)
{
    free(m->body); // STOP
    m->body = NULL;
    m->len = 0;
}

int main(void)
{
    static const char *const wire[] = {"1:5:hello", "2:4:ab\tc", "3:3:bye"};
    unsigned accepted = 0, rejected = 0;
    size_t bytes = 0;
    for (size_t i = 0; i < 3; i++) {
        struct msg m = {0, 0, NULL};
        if (msg_decode(&m, wire[i]) == 0) {
            accepted++;
            bytes += m.len;
        } else {
            rejected++;
        }
        msg_clear(&m);
    }
    printf("%u accepted (%zu bytes), %u rejected\n", accepted, bytes, rejected);
    return accepted == 2 && rejected == 1 && bytes == 8 ? 0 : 1;
}
