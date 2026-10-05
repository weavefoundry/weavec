// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Word counter on an open-addressing hash table. table_init() rounds the
 * requested size up to a power of two for the probe mask, but allocates only
 * the requested number of slots, so any hash that lands in [want, cap)
 * indexes past the slot array.
 * Category: spatial (heap buffer overflow, index computed from a hash).
 * Why it may be missed: mask and cap agree with each other; only calloc()
 * uses the size before rounding, far from where the slots are indexed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    const char *key;
    unsigned count;
};

struct table {
    struct entry *slots;
    size_t cap;
    size_t mask;
    size_t used;
};

static size_t round_pow2(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static int table_init(struct table *t, size_t want)
{
    size_t cap = round_pow2(want);
#ifdef FIX
    t->slots = calloc(cap, sizeof *t->slots);
#else
    t->slots = calloc(want, sizeof *t->slots);
#endif
    t->cap = cap;
    t->mask = cap - 1;
    t->used = 0;
    return t->slots ? 0 : -1;
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static struct entry *table_slot(struct table *t, const char *key)
{
    size_t i = hash_str(key) & t->mask;
    for (;;) {
        struct entry *e = &t->slots[i];
        if (!e->key || strcmp(e->key, key) == 0) // STOP
            return e;
        i = (i + 1) & t->mask;
    }
}

static int count_word(struct table *t, const char *word)
{
    struct entry *e = table_slot(t, word);
    if (!e->key) {
        if (t->used + 1 > t->cap / 2)
            return -1; /* no resizing in this tool */
        e->key = word;
        t->used++;
    }
    e->count++;
    return 0;
}

int main(void)
{
    char text[] = "the cat and the dog and the bird saw a cat chase a dog";
    struct table t;
    if (table_init(&t, 20) != 0)
        return 1;
    unsigned words = 0;
    for (char *w = strtok(text, " "); w; w = strtok(NULL, " ")) {
        if (count_word(&t, w) != 0)
            break;
        words++;
    }
    unsigned total = 0, distinct = 0;
    for (size_t i = 0; i < t.cap; i++) {
        if (t.slots[i].key) {
            total += t.slots[i].count;
            distinct++;
        }
    }
    free(t.slots);
    printf("%u words, %u distinct\n", words, distinct);
    return words == 14 && total == 14 && distinct == 8 ? 0 : 1;
}
