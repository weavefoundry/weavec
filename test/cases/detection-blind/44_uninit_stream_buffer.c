// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Output streams allocated with malloc(). stream_open() sets up the buffer
 * pointer only for buffered streams; for an unbuffered stream s->buf is left
 * uninitialised, and stream_close() passes it to free() unconditionally.
 * Category: uninitialized (use of an uninitialised heap pointer, here freed).
 * Why it may be missed: the write path checks s->buffered before touching
 * buf, so only close uses the field, and fresh heap memory is often zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct stream {
    const char *name;
    int buffered;
    char *buf;
    size_t cap;
    size_t len;
    size_t written;
};

static struct stream *stream_open(const char *name, int buffered)
{
    struct stream *s = malloc(sizeof *s);
    if (!s)
        return NULL;
    s->name = name;
    s->buffered = buffered;
    s->len = 0;
    s->written = 0;
    if (buffered) {
        s->cap = 64;
        s->buf = malloc(s->cap);
        if (!s->buf) {
            free(s);
            return NULL;
        }
    }
#ifdef FIX
    else {
        s->cap = 0;
        s->buf = NULL;
    }
#endif
    return s;
}

static void stream_flush(struct stream *s)
{
    s->written += s->len;
    s->len = 0;
}

static void stream_write(struct stream *s, const char *data)
{
    size_t n = strlen(data);
    if (!s->buffered) {
        s->written += n;
        return;
    }
    if (s->len + n > s->cap)
        stream_flush(s);
    if (n > s->cap) {
        s->written += n;
        return;
    }
    memcpy(s->buf + s->len, data, n);
    s->len += n;
}

static size_t stream_close(struct stream *s)
{
    stream_flush(s);
    size_t w = s->written;
    free(s->buf); // STOP // MISS: zero-initialisation makes the never-set pointer null, and free(NULL) is defined (neutralised)
    free(s);
    return w;
}

int main(void)
{
    struct stream *out = stream_open("stdout", 1);
    struct stream *err = stream_open("stderr", 0);
    if (!out || !err)
        return 1;
    stream_write(out, "result: 42\n");
    stream_write(err, "warning: cache cold\n");
    stream_write(out, "done\n");
    size_t a = stream_close(out);
    size_t b = stream_close(err);
    printf("%zu + %zu bytes written\n", a, b);
    return a == 16 && b == 20 ? 0 : 1;
}
