// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Letter frequency counter. Letters are counted with counts[c - 'a'] after an
 * isalpha() test, but upper-case letters are not folded first, so 'S' gives
 * the index -14 and the increment lands before the stack array.
 * Category: spatial (stack buffer underflow, negative index from the data).
 * Why it may be missed: the isalpha() guard looks like the range check, and
 * all-lower-case test sentences never produce a negative index.
 */
#include <ctype.h>
#include <stdio.h>

static void count_letters(const char *text, unsigned *counts)
{
    for (const char *p = text; *p != '\0'; p++) {
        int c = (unsigned char)*p;
        if (!isalpha(c))
            continue;
#ifdef FIX
        c = tolower(c);
#endif
        counts[c - 'a']++; // STOP
    }
}

int main(void)
{
    unsigned counts[26] = {0};
    const char *text = "Sphinx of black quartz, judge my vow";
    count_letters(text, counts);
    int best = 0;
    unsigned total = 0;
    for (int i = 0; i < 26; i++) {
        total += counts[i];
        if (counts[i] > counts[best])
            best = i;
    }
    printf("%u letters, most frequent '%c' (%u)\n", total, 'a' + best, counts[best]);
    return total == 29 ? 0 : 1;
}
