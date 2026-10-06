// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Two-entry LRU cache whose get() returns a pointer into the cache entry.
 * The caller keeps two such pointers and then inserts a third key, which
 * evicts (frees) the least recently used entry; formatting the response then
 * reads the evicted entry's value.
 * Category: temporal (heap use-after-free through a borrowed pointer into a
 * container).
 * Why it may be missed: the eviction is a side effect of put(), and which
 * entry it frees depends on the access order a few lines earlier.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    int key;
    char value[24];
    struct entry *prev, *next;
};

struct cache {
    struct entry *head, *tail; /* head = most recently used */
    size_t len, cap;
};

static void unlink_entry(struct cache *c, struct entry *e)
{
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void push_front(struct cache *c, struct entry *e)
{
    e->prev = NULL;
    e->next = c->head;
    if (c->head) c->head->prev = e; else c->tail = e;
    c->head = e;
}

/* Returns the cached value; valid while the entry stays in the cache. */
static const char *cache_get(struct cache *c, int key)
{
    for (struct entry *e = c->head; e; e = e->next) {
        if (e->key == key) {
            unlink_entry(c, e);
            push_front(c, e);
            return e->value;
        }
    }
    return NULL;
}

static int cache_put(struct cache *c, int key, const char *value)
{
    if (c->len == c->cap) {
        struct entry *old = c->tail;
        unlink_entry(c, old);
        free(old);
        c->len--;
    }
    struct entry *e = malloc(sizeof *e);
    if (!e)
        return -1;
    e->key = key;
    snprintf(e->value, sizeof e->value, "%s", value);
    push_front(c, e);
    c->len++;
    return 0;
}

int main(void)
{
    struct cache c = {NULL, NULL, 0, 2};
    if (cache_put(&c, 1, "alpha") != 0 || cache_put(&c, 2, "beta") != 0)
        return 1;
#ifdef FIX
    char first[24], second[24];
    snprintf(first, sizeof first, "%s", cache_get(&c, 1));
    snprintf(second, sizeof second, "%s", cache_get(&c, 2));
#else
    const char *first = cache_get(&c, 1);
    const char *second = cache_get(&c, 2);
#endif
    if (cache_put(&c, 3, "gamma") != 0) /* fill the miss for key 3 */
        return 1;
    char line[64];
    snprintf(line, sizeof line, "%s,%s", first, second); // STOP
    printf("%s\n", line);
    while (c.head) {
        struct entry *e = c.head;
        unlink_entry(&c, e);
        free(e);
    }
    return strcmp(line, "alpha,beta") == 0 ? 0 : 1;
}
