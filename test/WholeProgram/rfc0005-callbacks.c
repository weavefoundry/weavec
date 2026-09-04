// RFC 0005: an indirect call's candidates include the address-taken
// functions of the same type in every unit of the program (RFC 0004 finds
// only this unit's). handlers.c registers `on_done`, which frees its
// argument; calling the handler here therefore frees.
//
// RUN: not %weavec --whole-program %s %S/Inputs/handlers.c -- 2>&1 | FileCheck %s
//
// Alone, the call through `h` has no candidates: it is a boundary, reported
// once per function type, and nothing is an error.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"

void (*get_handler(void))(void *);

// ALONE: warning: call to 'get_handler' is not checked
// ALONE: warning: call through 'h' is not checked: its function type has no ownership annotations and no function of that type has its address taken in this program [weavec::annotation-required]
// ALONE-NOT: error:

int run(void) {
  char *buf = malloc(4);
  if (!buf)
    return 0;
  void (*h)(void *) = get_handler();
  h(buf);
  // CHECK: rfc0005-callbacks.c:[[@LINE+1]]:10: error: use of 'buf' after it was freed [weavec::use-after-free]
  return buf[0];
}
