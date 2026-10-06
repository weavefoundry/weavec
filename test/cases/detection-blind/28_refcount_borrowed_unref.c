// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Reference-counted message buffers. audit() only borrows the buffer, but it
 * ends with buf_unref() as if it owned a reference. For audited messages the
 * count drops to zero and the buffer is freed; the caller then hands it to
 * the output queue, whose buf_ref() touches the freed buffer.
 * Category: temporal (heap use-after-free through a reference-count error).
 * Why it may be missed: each function looks balanced on its own; the extra
 * unref only matters for the messages that audit() is called on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buf {
    int refs;
    size_t len;
    char data[48];
};

static struct buf *buf_new(const char *s)
{
    struct buf *b = malloc(sizeof *b);
    if (!b)
        return NULL;
    b->refs = 1;
    snprintf(b->data, sizeof b->data, "%s", s);
    b->len = strlen(b->data);
    return b;
}

static void buf_ref(struct buf *b)
{
    b->refs++; // STOP // MISS: a callee's parameter is assumed live (A1); the liveness of the caller's argument at the call is not guarded
}

static void buf_unref(struct buf *b)
{
    if (--b->refs == 0)
        free(b);
}

struct outq {
    struct buf *items[8];
    size_t len;
};

static int outq_push(struct outq *q, struct buf *b)
{
    if (q->len == 8)
        return -1;
    buf_ref(b);
    q->items[q->len++] = b;
    return 0;
}

static size_t outq_drain(struct outq *q)
{
    size_t total = 0;
    for (size_t i = 0; i < q->len; i++) {
        total += q->items[i]->len;
        buf_unref(q->items[i]);
    }
    q->len = 0;
    return total;
}

static size_t audited_bytes;

/* Records a message in the audit log. Borrows b. */
static void audit(struct buf *b)
{
    audited_bytes += b->len;
#ifndef FIX
    buf_unref(b); /* done with it */
#endif
}

int main(void)
{
    static const char *const msgs[] = {"HELO relay", "MAIL FROM:<ops@example.org>", "QUIT"};
    struct outq q = {{NULL}, 0};
    for (size_t i = 0; i < 3; i++) {
        struct buf *b = buf_new(msgs[i]);
        if (!b)
            return 1;
        if (strncmp(b->data, "MAIL", 4) == 0)
            audit(b);
        if (outq_push(&q, b) != 0)
            return 1;
        buf_unref(b);
    }
    size_t sent = outq_drain(&q);
    printf("sent %zu bytes, audited %zu\n", sent, audited_bytes);
    return sent == 41 && audited_bytes == 27 ? 0 : 1;
}
