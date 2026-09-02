// RFC 0003: effects on file-scope variables are summarised like effects on
// parameters, so a helper that frees a global marks it freed in its callers.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

static char *cache;

static void cache_free(void) { free(cache); }
static void cache_reset(void) {
  free(cache);
  cache = NULL;
}

void freed_by_callee(void) {
  cache = malloc(8);
  cache_free();
  // CHECK: rfc0003-globals.c:[[@LINE+1]]:3: error: use of 'cache' after it was freed [weavec::use-after-free]
  cache[0] = 1;
  // CHECK: rfc0003-globals.c:[[@LINE-3]]:3: note: freed here
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
  // CHECK: rfc0003-globals.c:[[@LINE+1]]:3: error: 'cache' is freed twice [weavec::double-free]
  cache_free();
}

// CHECK: 2 errors generated.
