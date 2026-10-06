// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Chained hash map with an iterator that caches the bucket array. Adding a
 * backup entry for every key while iterating grows the map, which frees the
 * old bucket array; the iterator then reads its next bucket from it.
 * Category: temporal (heap use-after-free, iterator invalidation).
 * Why it may be missed: the nodes themselves survive the rehash, so the
 * iteration appears to work until the iterator moves to its next bucket.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct node {
    char key[16];
    int val;
    struct node *next;
};

struct map {
    struct node **buckets;
    size_t nb;
    size_t len;
};

struct iter {
    struct node **buckets;
    size_t nb;
    size_t idx;
    struct node *cur;
};

static size_t hash(const char *s)
{
    size_t h = 5381;
    while (*s)
        h = h * 33u + (unsigned char)*s++;
    return h;
}

static int map_resize(struct map *m)
{
    size_t nb = m->nb * 2;
    struct node **nbk = calloc(nb, sizeof *nbk);
    if (!nbk)
        return -1;
    for (size_t i = 0; i < m->nb; i++) {
        for (struct node *n = m->buckets[i], *next; n; n = next) {
            next = n->next;
            size_t b = hash(n->key) % nb;
            n->next = nbk[b];
            nbk[b] = n;
        }
    }
    free(m->buckets);
    m->buckets = nbk;
    m->nb = nb;
    return 0;
}

static int map_put(struct map *m, const char *key, int val)
{
    size_t b = hash(key) % m->nb;
    for (struct node *n = m->buckets[b]; n; n = n->next)
        if (strcmp(n->key, key) == 0) {
            n->val = val;
            return 0;
        }
    if (m->len + 1 > m->nb) {
        if (map_resize(m) != 0)
            return -1;
        b = hash(key) % m->nb;
    }
    struct node *n = malloc(sizeof *n);
    if (!n)
        return -1;
    snprintf(n->key, sizeof n->key, "%s", key);
    n->val = val;
    n->next = m->buckets[b];
    m->buckets[b] = n;
    m->len++;
    return 0;
}

static void iter_init(struct iter *it, const struct map *m)
{
    *it = (struct iter){m->buckets, m->nb, 0, NULL};
}

static struct node *iter_next(struct iter *it)
{
    if (it->cur)
        it->cur = it->cur->next;
    while (!it->cur && it->idx < it->nb)
        it->cur = it->buckets[it->idx++]; // STOP
    return it->cur;
}

static int is_backup(const char *key)
{
    size_t n = strlen(key);
    return n >= 4 && strcmp(key + n - 4, "_bak") == 0;
}

int main(void)
{
    static const char *const names[] = {"alpha", "beta", "gamma", "delta", "eps", "zeta"};
    struct map m = {calloc(8, sizeof(struct node *)), 8, 0};
    if (!m.buckets)
        return 1;
    for (int i = 0; i < 6; i++)
        if (map_put(&m, names[i], i) != 0)
            return 1;
    struct iter it;
    char bak[16];
#ifdef FIX
    char keys[16][16];
    int vals[16];
    size_t nk = 0;
    iter_init(&it, &m);
    for (struct node *n = iter_next(&it); n && nk < 16; n = iter_next(&it)) {
        if (is_backup(n->key))
            continue;
        snprintf(keys[nk], sizeof keys[nk], "%s", n->key);
        vals[nk++] = n->val;
    }
    for (size_t k = 0; k < nk; k++) {
        snprintf(bak, sizeof bak, "%s_bak", keys[k]);
        if (map_put(&m, bak, vals[k]) != 0)
            return 1;
    }
#else
    iter_init(&it, &m);
    for (struct node *n = iter_next(&it); n; n = iter_next(&it)) {
        if (is_backup(n->key))
            continue;
        snprintf(bak, sizeof bak, "%s_bak", n->key);
        if (map_put(&m, bak, n->val) != 0)
            return 1;
    }
#endif
    size_t len = m.len;
    for (size_t i = 0; i < m.nb; i++)
        for (struct node *n = m.buckets[i], *next; n; n = next) {
            next = n->next;
            free(n);
        }
    free(m.buckets);
    printf("%zu entries\n", len);
    return len == 12 ? 0 : 1;
}
