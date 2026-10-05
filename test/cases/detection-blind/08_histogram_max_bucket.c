// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Latency histogram with NBUCKETS equal-width buckets between the observed
 * minimum and maximum. The bucket of a value is (v - lo) * NBUCKETS / span,
 * which is NBUCKETS for the maximum itself, one past the global array.
 * Category: spatial (global buffer overflow, index computed from the data).
 * Why it may be missed: every value but the maximum lands in range, and the
 * index is a computation over data rather than a visible loop bound.
 */
#include <stdio.h>

#define NBUCKETS 8

static unsigned hist[NBUCKETS];

static void build_histogram(const unsigned *v, size_t n)
{
    unsigned lo = v[0], hi = v[0];
    for (size_t i = 1; i < n; i++) {
        if (v[i] < lo)
            lo = v[i];
        if (v[i] > hi)
            hi = v[i];
    }
    unsigned span = hi > lo ? hi - lo : 1;
    for (size_t i = 0; i < n; i++) {
        size_t b = (size_t)(v[i] - lo) * NBUCKETS / span;
#ifdef FIX
        if (b >= NBUCKETS)
            b = NBUCKETS - 1; /* the maximum belongs to the last bucket */
#endif
        hist[b]++; // STOP
    }
}

int main(void)
{
    static const unsigned latency_us[] = {120, 340, 95, 410, 220, 180,
                                          760, 130, 300, 255, 98, 615};
    size_t n = sizeof latency_us / sizeof latency_us[0];
    build_histogram(latency_us, n);
    unsigned total = 0;
    for (size_t b = 0; b < NBUCKETS; b++) {
        printf("bucket %zu: %u\n", b, hist[b]);
        total += hist[b];
    }
    return total == n ? 0 : 1;
}
