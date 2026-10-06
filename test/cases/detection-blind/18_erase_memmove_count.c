// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Removes negative readings from an exactly-sized heap array by shifting the
 * tail down with memmove(). The element count passed is n - i instead of
 * n - i - 1, so every erase reads one element past the end of the array.
 * Category: spatial (heap buffer over-read by 4 bytes, wrong memmove length).
 * Why it may be missed: the extra element is copied into the slot that is
 * about to be dropped from the count, so the visible result is correct.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void erase_at(int *a, size_t *n, size_t i)
{
#ifdef FIX
    memmove(&a[i], &a[i + 1], (*n - i - 1) * sizeof *a);
#else
    memmove(&a[i], &a[i + 1], (*n - i) * sizeof *a); // STOP
#endif
    (*n)--;
}

static size_t remove_negatives(int *a, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (a[i] < 0)
            erase_at(a, &n, i);
        else
            i++;
    }
    return n;
}

int main(void)
{
    static const int input[] = {4, -1, 7, -3, -8, 2, 9, -5};
    static const int want[] = {4, 7, 2, 9};
    int *a = malloc(sizeof input);
    if (!a)
        return 1;
    memcpy(a, input, sizeof input);
    size_t n = remove_negatives(a, sizeof input / sizeof input[0]);
    int ok = n == 4 && memcmp(a, want, sizeof want) == 0;
    for (size_t i = 0; i < n; i++)
        printf("%d ", a[i]);
    printf("\n");
    free(a);
    return ok ? 0 : 1;
}
