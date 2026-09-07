// RFC 0017: target width, signed wrapping mode and suppressed diagnostics.
// RUN: not %weavec %s -- -fwrapv -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec %s -- -target i386-unknown-linux-gnu -fwrapv -ferror-limit=0 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "weavec.h"

void unsigned_target(size_t n) {
  if(n != (size_t)-1) return;
  n++;
  int *p=malloc(sizeof *p); if(!p)return;
  free(p);
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  if(n==0) *p=1;
}

void signed_wrapping(int n) {
  if(n != 2147483647) return;
  n++;
  int *p=malloc(sizeof *p); if(!p)return;
  free(p);
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  if(n<0) *p=1;
}

int shift_is_still_invalid(int n) {
  if(n!=-1)return 0;
  // CHECK: error: invalid integer operation: invalid signed left shift [weavec::invalid-integer-operation]
  return n<<1;
}

void unsafe_effects(void) {
  unsigned char n=255;
  int *p=malloc(sizeof *p); if(!p)return;
  WEAVEC_UNSAFE { n++; free(p); }
  // CHECK: error: use of 'p' after it was freed [weavec::use-after-free]
  if(n==0) *p=1;
}

WEAVEC_UNSAFE int suppressed(int n) {
  if(n!=-1)return 0;
  return n<<1;
}
// CHECK: 4 errors generated.
