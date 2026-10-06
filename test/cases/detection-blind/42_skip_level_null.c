// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Org chart report that prints each employee's skip-level manager (the
 * manager's manager). skip_level() checks that the employee has a manager
 * but not that the manager has one, so for a direct report of the CEO it
 * dereferences a NULL manager pointer.
 * Category: null (NULL dereference on a data-dependent path).
 * Why it may be missed: there is a NULL check right above the dereference,
 * just one level short; most employees are two levels below the top.
 */
#include <stdio.h>

struct employee {
    const char *name;
    struct employee *manager; /* NULL for the CEO */
};

static const char *skip_level(const struct employee *e)
{
    if (!e->manager)
        return NULL;
#ifdef FIX
    if (!e->manager->manager)
        return NULL;
#endif
    return e->manager->manager->name; // STOP
}

int main(void)
{
    static const struct {
        const char *name;
        int manager; /* index into the roster, -1 for none */
    } roster[] = {{"Ada", -1}, {"Brian", 0}, {"Chen", 1}, {"Dana", 1}, {"Eve", 2}};
    enum { N = sizeof roster / sizeof roster[0] };
    struct employee staff[N];
    for (int i = 0; i < N; i++) {
        staff[i].name = roster[i].name;
        staff[i].manager = roster[i].manager >= 0 ? &staff[roster[i].manager] : NULL;
    }
    unsigned with_skip = 0;
    for (int i = N - 1; i >= 0; i--) {
        const char *s = skip_level(&staff[i]);
        printf("%-6s skip-level: %s\n", staff[i].name, s ? s : "-");
        if (s)
            with_skip++;
    }
    return with_skip == 3 ? 0 : 1;
}
