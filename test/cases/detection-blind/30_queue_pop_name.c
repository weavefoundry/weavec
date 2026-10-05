// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Job queue whose pop() returns the job's name, documented as valid until
 * the next pop. The implementation frees the node right away, so the name
 * the caller receives points into freed memory from the start.
 * Category: temporal (heap use-after-free, pointer into a freed node
 * returned across a function boundary).
 * Why it may be missed: pop() itself never reads the node after free(); it
 * only computes the address, and the read happens in the caller.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct job {
    char name[24];
    int prio;
    struct job *next;
};

struct jobq {
    struct job *head, *tail;
#ifdef FIX
    struct job *last; /* node of the last pop, kept until the next one */
#endif
};

static int jobq_push(struct jobq *q, const char *name, int prio)
{
    struct job *j = malloc(sizeof *j);
    if (!j)
        return -1;
    snprintf(j->name, sizeof j->name, "%s", name);
    j->prio = prio;
    j->next = NULL;
    if (q->tail)
        q->tail->next = j;
    else
        q->head = j;
    q->tail = j;
    return 0;
}

/* Returns the next job's name; it stays valid until the next call. */
static const char *jobq_pop(struct jobq *q)
{
    struct job *j = q->head;
    if (!j)
        return NULL;
    q->head = j->next;
    if (!q->head)
        q->tail = NULL;
#ifdef FIX
    free(q->last);
    q->last = j;
#else
    free(j);
#endif
    return j->name;
}

int main(void)
{
    static const char *const jobs[] = {"compile", "link", "test", "package"};
    struct jobq q = {0};
    for (int i = 0; i < 4; i++)
        if (jobq_push(&q, jobs[i], i) != 0)
            return 1;
    size_t total = 0;
    unsigned tests = 0;
    const char *name;
    while ((name = jobq_pop(&q)) != NULL) {
        total += strlen(name); // STOP
        if (strcmp(name, "test") == 0)
            tests++;
    }
#ifdef FIX
    free(q.last);
#endif
    printf("%zu characters, %u test jobs\n", total, tests);
    return total == 22 && tests == 1 ? 0 : 1;
}
