// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Sieve of Eratosthenes over a heap bitmap. bm_init() allocates nbits / 8
 * bytes, which rounds down: for 100 bits it allocates 12 bytes, and marking
 * any of bits 96..99 writes the 13th byte, one past the block.
 * Category: spatial (heap buffer overflow by 1 byte).
 * Why it may be missed: the bitmap works for every multiple of 8 bits, and
 * bm_set() checks its index against nbits, which looks like a full guard.
 */
#include <stdio.h>
#include <stdlib.h>

struct bitmap {
    unsigned char *bits;
    size_t nbits;
};

static int bm_init(struct bitmap *b, size_t nbits)
{
#ifdef FIX
    b->bits = calloc((nbits + 7) / 8, 1);
#else
    b->bits = calloc(nbits / 8, 1);
#endif
    b->nbits = nbits;
    return b->bits ? 0 : -1;
}

static void bm_set(struct bitmap *b, size_t i)
{
    if (i < b->nbits)
        b->bits[i / 8] |= (unsigned char)(1u << (i % 8)); // STOP
}

static int bm_test(const struct bitmap *b, size_t i)
{
    return i < b->nbits && ((b->bits[i / 8] >> (i % 8)) & 1);
}

int main(void)
{
    enum { N = 100 };
    struct bitmap composite;
    if (bm_init(&composite, N) != 0)
        return 1;
    for (size_t p = 2; p * p < N; p++)
        if (!bm_test(&composite, p))
            for (size_t m = p * p; m < N; m += p)
                bm_set(&composite, m);
    unsigned primes = 0;
    for (size_t i = 2; i < N; i++)
        if (!bm_test(&composite, i))
            primes++;
    free(composite.bits);
    printf("%u primes below %d\n", primes, N);
    return primes == 25 ? 0 : 1;
}
