/*
 * Driver for the sds-df-freesplitres injection
 * (test/corpus/injections/injections.json): sds-test never splits a string,
 * so the gate builds this with sds.c from the patched copy and runs it.
 * It splits a string and frees the result, which reaches the injected
 * second release of the token array in sdsfreesplitres(). Unpatched, it
 * prints nothing and exits 0.
 */
#include <stdio.h>
#include <string.h>

#include "sds.h"

int main(void) {
    int count = 0;
    sds *tokens = sdssplitlen("a,b,c", 5, ",", 1, &count);

    if (tokens == NULL || count != 3) {
        fprintf(stderr, "driver: sdssplitlen returned %d tokens\n", count);
        return 1;
    }
    sdsfreesplitres(tokens, count);
    return 0;
}
