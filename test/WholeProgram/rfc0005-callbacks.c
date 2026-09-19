// RFC 0005: an indirect call's candidates include the address-taken
// functions of the same type in every unit of the program (RFC 0004 finds
// only this unit's). handlers.c registers `on_done`, which frees its
// argument; calling the handler here therefore frees.
//
// RUN: not %weavec --whole-program %s %S/Inputs/handlers.c -- 2>&1 | FileCheck %s
//
// Alone, the call through `h` has no candidates: it is a call into unknown
// code (RFC 0030 §5.1), a ledger row, and nothing is reported.
// RUN: %weavec --ledger=%t.json %s -- 2>&1 | FileCheck --check-prefix=ALONE %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include "../Inputs/prelude.h"

void (*get_handler(void))(void *);

// ALONE-NOT: {{warning|error}}:
// ALONE: 0 errors, 0 warnings
// LEDGER: "text": "get_handler()",
// LEDGER: "reason": "unknown-callee",
// LEDGER: "text": "h(buf)",
// LEDGER: "reason": "unknown-callee",
// LEDGER-NEXT: "detail": "the target of 'h' is unknown; annotate the parameters of its function type",

int run(void) {
  char *buf = malloc(4);
  if (!buf)
    return 0;
  void (*h)(void *) = get_handler();
  h(buf);
  // CHECK: rfc0005-callbacks.c:[[@LINE+1]]:10: error: use of 'buf' after it was freed [weavec::use-after-free]
  return buf[0];
}
