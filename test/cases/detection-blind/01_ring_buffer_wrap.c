// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Ring buffer of trace records. The wrap test after a push uses `>` instead
 * of `>=`, so the write index reaches cap and one record is stored past the
 * end of the slot array before the index wraps.
 * Category: spatial (heap buffer overflow by one 16-byte element).
 * Why it may be missed: the wrap looks like an ordinary bounds check, and the
 * overflow only happens on the cap-th push, after the buffer looked healthy.
 */
#include <stdio.h>
#include <stdlib.h>

struct record {
    unsigned seq;
    char tag[12];
};

struct ring {
    struct record *slots;
    size_t cap;
    size_t head;
    size_t count;
};

static int ring_init(struct ring *rb, size_t cap)
{
    rb->slots = malloc(cap * sizeof *rb->slots);
    rb->cap = cap;
    rb->head = 0;
    rb->count = 0;
    return rb->slots ? 0 : -1;
}

static void ring_push(struct ring *rb, unsigned seq, const char *tag)
{
    struct record *r = &rb->slots[rb->head];
    r->seq = seq; // STOP
    snprintf(r->tag, sizeof r->tag, "%s", tag);
    rb->head++;
#ifdef FIX
    if (rb->head >= rb->cap)
#else
    if (rb->head > rb->cap)
#endif
        rb->head = 0;
    if (rb->count < rb->cap)
        rb->count++;
}

static unsigned ring_checksum(const struct ring *rb)
{
    unsigned sum = 0;
    for (size_t i = 0; i < rb->count; i++)
        if (rb->slots[i].tag[0] != '\0')
            sum += rb->slots[i].seq;
    return sum;
}

int main(void)
{
    static const char *const tags[] = {"open", "read", "write", "close"};
    struct ring rb;
    if (ring_init(&rb, 8) != 0)
        return 1;
    for (unsigned i = 0; i < 20; i++)
        ring_push(&rb, i, tags[i % 4]);
    unsigned sum = ring_checksum(&rb);
    free(rb.slots);
    /* The last 8 records are 12..19. */
    return sum == 124 ? 0 : 1;
}
