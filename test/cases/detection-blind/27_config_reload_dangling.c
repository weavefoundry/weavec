// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Configuration reload. config_reload() clears the old configuration (which
 * frees its strings) before parsing the new text; when parsing fails, the
 * error message reports the old host through the pointer it just freed.
 * Category: temporal (heap use-after-free across a helper function).
 * Why it may be missed: config_clear() reads as a harmless reset, and the
 * free happens inside it, two calls away from the use on the error path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct config {
    char *host;
    char *user;
    unsigned port;
};

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void config_clear(struct config *c)
{
    free(c->host);
    free(c->user);
    c->port = 0;
}

/* Parses "host user port"; leaves c untouched on error. */
static int config_parse(struct config *c, const char *text)
{
    char host[64], user[32];
    unsigned port;
    if (sscanf(text, "%63s %31s %u", host, user, &port) != 3 || port == 0 || port > 65535)
        return -1;
    char *h = dupstr(host), *u = dupstr(user);
    if (!h || !u) {
        free(h);
        free(u);
        return -1;
    }
    c->host = h;
    c->user = u;
    c->port = port;
    return 0;
}

static int config_reload(struct config *c, const char *text)
{
#ifdef FIX
    struct config fresh = {NULL, NULL, 0};
    if (config_parse(&fresh, text) != 0) {
        fprintf(stderr, "reload failed, keeping %s\n", c->host);
        return -1;
    }
    config_clear(c);
    *c = fresh;
    return 0;
#else
    config_clear(c);
    if (config_parse(c, text) != 0) {
        fprintf(stderr, "reload failed, keeping %s\n", c->host); // STOP
        return -1;
    }
    return 0;
#endif
}

int main(void)
{
    struct config c = {NULL, NULL, 0};
    if (config_parse(&c, "db.internal admin 5432") != 0)
        return 1;
    int failures = 0;
    if (config_reload(&c, "db2.internal admin") != 0) /* port missing */
        failures++;
    if (config_reload(&c, "db3.internal ops 6432") != 0)
        failures++;
    printf("%s@%s:%u\n", c.user, c.host, c.port);
    int ok = failures == 1 && strcmp(c.host, "db3.internal") == 0 && c.port == 6432;
    config_clear(&c);
    return ok ? 0 : 1;
}
