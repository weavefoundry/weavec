// Engine pin converted from test/Analysis/rfc0003-globals.c; markers are the v0.10.0 golden diagnostics.
// RFC 0003: effects on file-scope variables are summarised like effects on
// parameters, so a helper that frees a global marks it freed in its callers.
#include "Inputs/prelude.h"

static char *cache;

static void cache_free(void) { free(cache); }
static void cache_reset(void) {
  free(cache);
  cache = NULL;
}

void freed_by_callee(void) {
  cache = malloc(8);
  cache_free();
  cache[0] = 1; // BUG: use-after-free
}

void reset_is_fine(void) {
  cache = malloc(8);
  cache_reset();
  cache = malloc(8);
  free(cache);
}

void double_free_by_callee(void) {
  cache = malloc(8);
  free(cache);
  cache_free(); // BUG: double-free
}
