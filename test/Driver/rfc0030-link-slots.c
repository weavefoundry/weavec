// RFC 0030 §9.3, §13.2 step 2: `weavec --whole-program` solves the
// function-pointer slots of every unit together. Lua's allocator hook: one
// unit stores its caller's allocator into `g->frealloc` and frees through
// it, another passes its own `l_alloc`. Each unit alone sees an open field;
// the program sees the field closed with the one target `l_alloc`, and the
// engine's runs receive that solution (`ProgramFacts`), so the free through
// the field is a call of `l_alloc` there.
//
// RUN: %weavec --whole-program --dump-analysis %s %S/Inputs/rfc0030-hook-state.c -- -I%S/Inputs 2>/dev/null | FileCheck --check-prefix=CLOSED %s
#include <stdlib.h>
#include "rfc0030-hook-state.h"

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  (void)osize;
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, nsize);
}

int main(void) {
  struct global_state *g = new_state(l_alloc, NULL);
  char *p = malloc(8);
  if (!p)
    return 1;
  p[0] = 1;
  state_release(g, p, 8);
  return 0;
}

// CLOSED: program slots:
// CLOSED: field struct global_state frealloc: {{[{].*}}rfc0030-link-slots.c:l_alloc} closed
// CLOSED: param new_state 0: {{[{].*}}rfc0030-link-slots.c:l_alloc} closed

