// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Message bus with a callback registry. Each session subscribes a callback
 * with itself as the context. session_close() frees the session but leaves
 * its subscription in place, so the next broadcast calls the callback with
 * a dangling context, which updates the freed session.
 * Category: temporal (heap use-after-free through a callback context).
 * Why it may be missed: the free and the use are in unrelated functions, and
 * the only link between them is a void * stored in a global table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*handler_fn)(void *ctx, const char *msg);

struct listener {
    handler_fn fn;
    void *ctx;
    int active;
};

#define MAX_LISTENERS 8
static struct listener listeners[MAX_LISTENERS];

static int subscribe(handler_fn fn, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!listeners[i].active) {
            listeners[i].fn = fn;
            listeners[i].ctx = ctx;
            listeners[i].active = 1;
            return i;
        }
    }
    return -1;
}

static void unsubscribe(int id)
{
    if (id >= 0 && id < MAX_LISTENERS)
        listeners[id].active = 0;
}

static void publish(const char *msg)
{
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (listeners[i].active)
            listeners[i].fn(listeners[i].ctx, msg);
}

struct session {
    char user[16];
    size_t bytes_seen;
    int sub;
};

static void on_broadcast(void *ctx, const char *msg)
{
    struct session *s = ctx;
    s->bytes_seen += strlen(msg); // STOP
}

static struct session *session_open(const char *user)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    snprintf(s->user, sizeof s->user, "%s", user);
    s->sub = subscribe(on_broadcast, s);
    if (s->sub < 0) {
        free(s);
        return NULL;
    }
    return s;
}

static void session_close(struct session *s)
{
#ifdef FIX
    unsubscribe(s->sub);
#endif
    free(s);
}

int main(void)
{
    struct session *a = session_open("alice");
    struct session *b = session_open("bob");
    if (!a || !b)
        return 1;
    publish("server restarting");
    session_close(a);
    publish("back online");
    size_t seen = b->bytes_seen;
    printf("%s saw %zu bytes\n", b->user, seen);
    session_close(b);
    for (int i = 0; i < MAX_LISTENERS; i++) /* tear down the bus */
        unsubscribe(i);
    return seen == 28 ? 0 : 1;
}
