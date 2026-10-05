// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Purges idle sessions from a singly linked list. The loop frees a node in
 * its body and then the for-loop increment reads s->next from the node it
 * has just freed.
 * Category: temporal (heap use-after-free).
 * Why it may be missed: the read of the freed node hides in the for-header
 * increment, and the unlinking in the body is otherwise correct.
 */
#include <stdio.h>
#include <stdlib.h>

struct session {
    int id;
    unsigned last_seen;
    struct session *next;
};

static struct session *session_add(struct session *head, int id, unsigned last_seen)
{
    struct session *s = malloc(sizeof *s);
    if (!s)
        return head;
    s->id = id;
    s->last_seen = last_seen;
    s->next = head;
    return s;
}

static unsigned purge_idle(struct session **head, unsigned now, unsigned timeout)
{
    struct session **link = head;
    unsigned purged = 0;
#ifdef FIX
    for (struct session *s = *head, *next; s; s = next) {
        next = s->next;
#else
    for (struct session *s = *head; s; s = s->next) { // STOP
#endif
        if (now - s->last_seen > timeout) {
            *link = s->next;
            free(s);
            purged++;
        } else {
            link = &s->next;
        }
    }
    return purged;
}

int main(void)
{
    static const unsigned seen[] = {99, 10, 95, 20, 100};
    struct session *head = NULL;
    for (int i = 0; i < 5; i++)
        head = session_add(head, i, seen[i]);
    unsigned purged = purge_idle(&head, 100, 30);
    unsigned left = 0;
    while (head) {
        struct session *next = head->next;
        printf("session %d active\n", head->id);
        free(head);
        head = next;
        left++;
    }
    return purged == 2 && left == 3 ? 0 : 1;
}
