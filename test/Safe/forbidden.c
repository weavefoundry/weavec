// RFC 0041 §3, rules no-asm and unsafe-block, and bad-annotation.
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <setjmp.h>
#include <stddef.h>

static jmp_buf there;

WEAVEC_SAFE void assembly(void) {
  __asm__ volatile("");           // expected-error {{inline assembly cannot be in a safe function [weavec-safe:no-asm]}}
}

WEAVEC_SAFE int jumps(void) {
  if (setjmp(there))              // expected-error {{'setjmp' cannot be in a safe function [weavec-safe:no-asm]}}
    return 1;
  longjmp(there, 1);              // expected-error {{'longjmp' cannot be in a safe function}}
}

WEAVEC_SAFE void *frame(void) {
  return __builtin_frame_address(0); // expected-error {{[weavec-safe:no-asm]}}
}

WEAVEC_SAFE int block(int x) {
  WEAVEC_UNSAFE {                 // expected-error {{a WEAVEC_UNSAFE block cannot be inside a safe function [weavec-safe:unsafe-block]}}
    x++;
  }
  return x;
}

WEAVEC_SAFE int typo(const int *WEAVEC_COUNTED_BY(m) v, size_t n) { // expected-error {{'m' in the bound names no integer parameter of 'typo' [weavec-safe:bad-annotation]}}
  return v[0] + (int)n;
}
