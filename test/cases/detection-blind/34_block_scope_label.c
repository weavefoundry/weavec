// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Item description formatter. The label for a numbered item is formatted
 * into a buffer declared inside the else-block, and the pointer to it is
 * used after the block has ended, when the buffer's lifetime is over.
 * Category: temporal (stack use after scope).
 * Why it may be missed: the buffer is in the same function and usually still
 * holds the right bytes, so the output looks correct in testing.
 */
#include <stdio.h>
#include <string.h>

struct item {
    int id; /* -1 when not yet assigned */
    unsigned stock;
};

static int describe(const struct item *it, char *out, size_t cap)
{
    const char *label;
#ifdef FIX
    char tmp[16];
#endif
    if (it->id < 0) {
        label = "unassigned";
    } else {
#ifndef FIX
        char tmp[16];
#endif
        snprintf(tmp, sizeof tmp, "item-%d", it->id);
        label = tmp;
    }
    return snprintf(out, cap, "%s (%u in stock)", label, it->stock); // STOP // MISS: a stack object is untracked once its scope ends (RFC 0032 §4); it stops only where the frame puts the buffer one past a live object (darwin-arm64, linux-arm64)
}

int main(void)
{
    static const struct item items[] = {{7, 3}, {-1, 0}, {12, 40}};
    char line[64];
    int ok = 1;
    for (size_t i = 0; i < 3; i++) {
        if (describe(&items[i], line, sizeof line) < 0)
            return 1;
        puts(line);
        if (i == 0 && strcmp(line, "item-7 (3 in stock)") != 0)
            ok = 0;
    }
    return ok ? 0 : 1;
}
