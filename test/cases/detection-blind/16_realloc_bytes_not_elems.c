// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Growable series of samples. When the series is full it grows with
 * realloc(s->v, ncap): ncap is an element count but realloc() takes bytes,
 * so the block holds ncap bytes instead of ncap doubles and the very first
 * store writes 4 bytes past it.
 * Category: spatial (heap buffer overflow, wrong allocation size).
 * Why it may be missed: the capacity bookkeeping is consistent with itself;
 * only the missing `* sizeof *s->v` in the realloc() call is wrong.
 */
#include <stdio.h>
#include <stdlib.h>

struct series {
    double *v;
    size_t len;
    size_t cap;
};

static int series_push(struct series *s, double x)
{
    if (s->len == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4;
#ifdef FIX
        double *p = realloc(s->v, ncap * sizeof *s->v);
#else
        double *p = realloc(s->v, ncap);
#endif
        if (!p)
            return -1;
        s->v = p;
        s->cap = ncap;
    }
    s->v[s->len++] = x; // STOP
    return 0;
}

static double moving_average(const struct series *s, size_t window)
{
    if (window > s->len)
        window = s->len;
    if (window == 0)
        return 0;
    double sum = 0;
    for (size_t i = s->len - window; i < s->len; i++)
        sum += s->v[i];
    return sum / (double)window;
}

int main(void)
{
    struct series s = {NULL, 0, 0};
    for (int i = 1; i <= 10; i++) {
        if (series_push(&s, (double)i) != 0) {
            free(s.v);
            return 1;
        }
    }
    double avg = moving_average(&s, 4);
    printf("moving average of the last 4: %.2f\n", avg);
    free(s.v);
    return avg == 8.5 ? 0 : 1;
}
