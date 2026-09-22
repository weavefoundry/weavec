/*
 * Driver for the printf injection (test/corpus/injections/injections.json).
 * The gate builds it with printf.c from the patched copy and runs it.
 *
 * A precision of 10 is clamped to 9 by _ftoa; with the injected
 * off-by-one bound it is not, and _ftoa reads pow10[10] (printf-oob-pow10).
 * Unpatched, it prints the formatted value and exits 0.
 */
#include <stdio.h>
#include <string.h>

#include "printf.h"

void _putchar(char character) {
    putchar(character);
}

int main(void) {
    char buffer[64];
    int length = snprintf(buffer, sizeof(buffer), "%.10f", 3.25);

    if (length <= 0 || strncmp(buffer, "3.25", 4) != 0) {
        return 1;
    }
    puts(buffer);
    return 0;
}
