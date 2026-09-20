// RFC 0030 §4 "Trusted, unsafe": raw operations inside WEAVEC_UNSAFE are trusted, not checked.
// STAGE: S3
// The IntToPtr site's spatial and temporal facets are trusted(unsafe). 'reg' is an RFC 0004
// raw pointer (converted from an integer), so *reg is a Raw site whose spatial, null and
// temporal facets are all trusted(unsafe). No checks are emitted. Outside the region *reg
// is an unsafe-operation error (unsafe/raw-outside-region.c).
// CLEAN
// EXPECT-LEDGER: /summary/checked == 0
#include <stdint.h>
#include <weavec.h>

void poke(uintptr_t addr) {
  WEAVEC_UNSAFE {
    volatile unsigned *reg = (volatile unsigned *)addr; // TRUSTED: spatial:unsafe // TRUSTED: temporal:unsafe
    *reg = 1; // TRUSTED: spatial:unsafe // TRUSTED: null:unsafe // TRUSTED: temporal:unsafe
  }
}
