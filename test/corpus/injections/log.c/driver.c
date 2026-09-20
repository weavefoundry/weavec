/*
 * Driver for the log.c injections (test/corpus/injections/injections.json).
 * The gate builds it with src/log.c from the patched copy and runs it.
 *
 * It logs once with no lock set and every callback slot in use, which
 * reaches both injected bugs: lock() calling the null lock callback
 * (logc-null-lock) and log_log's loop reading past the callback table
 * (logc-oob-callbacks). Unpatched, it prints nothing and exits 0.
 */
#include <stdio.h>

#include "log.h"

static int calls;

static void count_event(log_Event *ev) {
    (void)ev;
    calls++;
}

int main(void) {
    int i;

    log_set_quiet(true);
    for (i = 0; i < 32; i++) {
        if (log_add_callback(count_event, NULL, LOG_TRACE) != 0) {
            fprintf(stderr, "driver: callback %d was not registered\n", i);
            return 1;
        }
    }
    log_info("every callback slot is in use");
    if (calls != 32) {
        fprintf(stderr, "driver: %d callbacks ran, expected 32\n", calls);
        return 1;
    }
    return 0;
}
