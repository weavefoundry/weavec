// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Fixed-capacity vector embedded in a struct, followed by a running checksum.
 * sv_push() rejects a push only when len > SV_CAP, so the ninth push writes
 * items[8], which is the checksum field, and the checksum is then corrupted.
 * Category: spatial (intra-object overflow by 4 bytes into the next field).
 * Why it may be missed: the bad store stays inside the heap object, so
 * allocator-level tools see nothing; the symptom is a wrong checksum later.
 */
#include <stdio.h>
#include <stdlib.h>

#define SV_CAP 8

struct smallvec {
    int len;
    int items[SV_CAP];
    int checksum; /* sum of items, maintained on every push */
};

static int sv_push(struct smallvec *v, int x)
{
#ifdef FIX
    if (v->len >= SV_CAP)
#else
    if (v->len > SV_CAP)
#endif
        return -1;
    v->items[v->len++] = x; // STOP
    v->checksum += x;
    return 0;
}

static int sv_verify(const struct smallvec *v)
{
    int s = 0;
    for (int i = 0; i < v->len; i++)
        s += v->items[i];
    return s == v->checksum;
}

int main(void)
{
    const char *input = "3 1 4 1 5 9 2 6 5";
    struct smallvec *v = calloc(1, sizeof *v);
    if (!v)
        return 1;
    unsigned dropped = 0;
    const char *p = input;
    while (*p) {
        int x = 0;
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            x = x * 10 + (*p++ - '0');
        if (sv_push(v, x) != 0)
            dropped++;
    }
    int ok = sv_verify(v);
    printf("%d readings kept, %u dropped, checksum %s\n", v->len, dropped, ok ? "ok" : "BAD");
    free(v);
    return ok ? 0 : 1;
}
