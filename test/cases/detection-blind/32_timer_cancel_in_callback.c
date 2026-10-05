// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Event loop with a list of timers. A one-shot timeout callback cancels its
 * own timer, which unlinks and frees it; when the callback returns, the loop
 * advances with t->next read from the freed timer.
 * Category: temporal (heap use-after-free, free inside a callback).
 * Why it may be missed: run_due() never frees anything itself; the free is
 * in a callback reached through a function pointer.
 */
#include <stdio.h>
#include <stdlib.h>

struct loop;

struct timer {
    unsigned due;
    unsigned period;
    void (*fire)(struct loop *, struct timer *);
    struct timer *next;
};

struct loop {
    struct timer *timers;
    unsigned now;
    unsigned ticks;
};

static struct timer *timer_add(struct loop *l, unsigned due, unsigned period,
                               void (*fire)(struct loop *, struct timer *))
{
    struct timer *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->due = due;
    t->period = period;
    t->fire = fire;
    t->next = l->timers;
    l->timers = t;
    return t;
}

static void timer_cancel(struct loop *l, struct timer *t)
{
    for (struct timer **p = &l->timers; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            free(t);
            return;
        }
    }
}

static void on_heartbeat(struct loop *l, struct timer *t)
{
    l->ticks++;
    t->due += t->period;
}

static void on_timeout(struct loop *l, struct timer *t)
{
    l->ticks += 10;
    timer_cancel(l, t); /* one-shot */
}

static void run_due(struct loop *l)
{
    struct timer *t = l->timers;
    while (t) {
#ifdef FIX
        struct timer *next = t->next;
#endif
        if (t->due <= l->now)
            t->fire(l, t);
#ifdef FIX
        t = next;
#else
        t = t->next; // STOP
#endif
    }
}

int main(void)
{
    struct loop l = {NULL, 0, 0};
    if (!timer_add(&l, 1, 1, on_heartbeat) || !timer_add(&l, 3, 0, on_timeout))
        return 1;
    for (l.now = 1; l.now <= 5; l.now++)
        run_due(&l);
    while (l.timers)
        timer_cancel(&l, l.timers);
    printf("%u ticks\n", l.ticks);
    return l.ticks == 15 ? 0 : 1;
}
