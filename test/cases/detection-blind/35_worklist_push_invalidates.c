// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Breadth-first expansion over a growable worklist. The loop takes a pointer
 * to the current task, pushes its children (which may realloc the array),
 * and then marks the task visited through the pointer taken before the push.
 * Category: temporal (heap use after realloc moved the block, iterator
 * invalidation).
 * Why it may be missed: the loop uses an index, which is safe, so the stale
 * pointer `t` looks like a harmless shorthand for w.items[i].
 */
#include <stdio.h>
#include <stdlib.h>

struct task {
    unsigned id;
    unsigned depth;
    int visited;
};

struct worklist {
    struct task *items;
    size_t len;
    size_t cap;
};

static int wl_push(struct worklist *w, unsigned id, unsigned depth)
{
    if (w->len == w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 4;
        struct task *p = realloc(w->items, ncap * sizeof *p);
        if (!p)
            return -1;
        w->items = p;
        w->cap = ncap;
    }
    w->items[w->len].id = id;
    w->items[w->len].depth = depth;
    w->items[w->len].visited = 0;
    w->len++;
    return 0;
}

int main(void)
{
    struct worklist w = {NULL, 0, 0};
    if (wl_push(&w, 1, 0) != 0)
        return 1;
    unsigned long idsum = 0;
    for (size_t i = 0; i < w.len; i++) {
        struct task *t = &w.items[i];
        unsigned id = t->id, depth = t->depth;
        if (depth < 3) {
            if (wl_push(&w, id * 2, depth + 1) != 0 || wl_push(&w, id * 2 + 1, depth + 1) != 0) {
                free(w.items);
                return 1;
            }
#ifdef FIX
            t = &w.items[i]; /* the pushes may have moved the array */
#endif
        }
        t->visited = 1; // STOP
        idsum += id;
    }
    size_t visited = 0;
    for (size_t i = 0; i < w.len; i++)
        visited += w.items[i].visited != 0;
    printf("%zu tasks, %zu visited, id sum %lu\n", w.len, visited, idsum);
    int ok = w.len == 15 && visited == 15 && idsum == 120;
    free(w.items);
    return ok ? 0 : 1;
}
