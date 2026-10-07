// RFC 0030 §4 "Trusted, unsafe": raw operations inside WEAVEC_UNSAFE are trusted, not checked.
// STAGE: S3
// The IntToPtr site's spatial and temporal facets are trusted(unsafe). 'reg' is converted
// from an integer (since RFC 0033 §2 a pointer of unknown provenance, not a raw one), and
// inside the region every facet of *reg is trusted(unsafe), as for a raw pointer. No checks
// are emitted. Outside the region *reg is guarded (unsafe/raw-outside-region.c).
// CLEAN
#include <stdint.h>
#include <weavec.h>

void poke(uintptr_t addr) {
  WEAVEC_UNSAFE {
    volatile unsigned *reg = (volatile unsigned *)addr;
    *reg = 1;
  }
}
