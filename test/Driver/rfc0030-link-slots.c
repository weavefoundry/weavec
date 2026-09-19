// RFC 0030 §9.3, §13.2 step 2: the link step solves the function-pointer
// slots of every unit record together. Lua's allocator hook: one unit
// stores its caller's allocator into `g->frealloc` and frees through it,
// another passes its own `l_alloc`. Each unit alone sees an open field; the
// linked program sees the field closed with the one target `l_alloc`, and
// the engine's runs at link receive that solution (`ProgramFacts`), so the
// free through the field is a call of `l_alloc` there. An input without a
// record could store into the field too, so with one the field stays open.
// `weavec --whole-program` solves the same slots from its units.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/rfc0030-hook-state.c -o %t/state.o -I%S/Inputs
// RUN: %weavec_cc -c %s -o %t/main.o -I%S/Inputs
// RUN: %weavec --dump-record=%t/state.o.weavec | FileCheck --check-prefix=RECORD %s
// RUN: %weavec_cc -fweavec-dump-analysis %t/state.o %t/main.o -o %t/prog 2>/dev/null | FileCheck --check-prefix=CLOSED %s
// RUN: %weavec --whole-program --dump-analysis %s %S/Inputs/rfc0030-hook-state.c -- -I%S/Inputs 2>/dev/null | FileCheck --check-prefix=CLOSED %s
//
// RUN: %clang -c %s -DOTHER -I%S/Inputs -o %t/other.o
// RUN: %weavec_cc -fweavec-dump-analysis %t/state.o %t/main.o %t/other.o -o %t/prog2 2>/dev/null | FileCheck --check-prefix=OPEN %s
#include <stdlib.h>
#include "rfc0030-hook-state.h"

#ifdef OTHER
int other(void) { return 0; }
#else
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
#endif

// The record exports the store of the parameter into the field.
// RECORD: "slots": [
// RECORD: "slot": "field struct global_state frealloc",
// RECORD-NEXT: "targets": [],
// RECORD-NEXT: "sources": [
// RECORD-NEXT: "param new_state 0"

// CLOSED: program slots:
// CLOSED: field struct global_state frealloc: {{[{].*}}rfc0030-link-slots.c:l_alloc} closed
// CLOSED: param new_state 0: {{[{].*}}rfc0030-link-slots.c:l_alloc} closed

// OPEN: program slots:
// OPEN: field struct global_state frealloc: {{[{].*}}rfc0030-link-slots.c:l_alloc} open ('struct global_state.frealloc' may be stored by code outside the analysed program)
