// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * HTTP-ish request handler whose strings live in a per-request arena that is
 * reset between requests. The connection remembers the Host of its first
 * request by keeping the arena pointer, so from the second request on it
 * compares against memory that the arena has handed out again.
 * Category: temporal (use of a stale pointer into a recycled arena block).
 * Why it may be missed: the arena's backing block is never freed, so the
 * pointer stays "valid" to the allocator; only the arena's reset ends it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct arena {
    char *base;
    size_t size;
    size_t used;
};

static void *arena_alloc(struct arena *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (n > a->size - a->used)
        return NULL;
    void *p = a->base + a->used;
    a->used += n;
    return p;
}

static void arena_reset(struct arena *a) { a->used = 0; }

static char *arena_strndup(struct arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

struct conn {
    const char *host; /* Host of the first request; later ones must match */
#ifdef FIX
    char host_buf[64];
#endif
    unsigned served;
};

/* Parses "METHOD PATH\nHost: name\n"; all strings go into the arena. */
static int handle_request(struct arena *a, struct conn *c, const char *raw)
{
    const char *nl = strchr(raw, '\n');
    if (!nl)
        return -1;
    char *reqline = arena_strndup(a, raw, (size_t)(nl - raw));
    const char *h = strstr(nl + 1, "Host: ");
    if (!reqline || !h)
        return -1;
    h += 6;
    char *host = arena_strndup(a, h, strcspn(h, "\n"));
    if (!host)
        return -1;
    if (!c->host) {
#ifdef FIX
        snprintf(c->host_buf, sizeof c->host_buf, "%s", host);
        c->host = c->host_buf;
#else
        c->host = host;
#endif
    } else if (strcmp(c->host, host) != 0) { // STOP // MISS: the application's arena keeps its block allocated, so a stale pointer into it reads live memory
        fprintf(stderr, "host mismatch on '%s'\n", reqline);
        return -1;
    }
    c->served++;
    return 0;
}

int main(void)
{
    static const char *const reqs[] = {
        "GET /index.html\nHost: example.org\n",
        "GET /assets/style.css\nHost: example.org\n",
        "GET /logo.png\nHost: example.org\n",
    };
    struct arena a = {malloc(256), 256, 0};
    struct conn c = {0};
    if (!a.base)
        return 1;
    int failed = 0;
    for (size_t i = 0; i < 3; i++) {
        arena_reset(&a);
        if (handle_request(&a, &c, reqs[i]) != 0)
            failed++;
    }
    printf("served %u requests\n", c.served);
    free(a.base);
    return failed == 0 && c.served == 3 ? 0 : 1;
}
