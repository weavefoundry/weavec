// RFC 0030 §6.1 and §2.3, as amended by RFC 0033 §2: outside a region a Raw site is an unsafe-operation error; a pointer converted from an integer is no Raw site.
// STAGE: S3
// 'reg' is declared WEAVEC_RAW: dereferencing it outside an unsafe region is the
// unsafe-operation error of RFC 0004. 'mmio' is converted from an integer, which since
// RFC 0033 makes a pointer of unknown provenance, not a raw one: the IntToPtr site's own
// facets are unresolved(raw-cast) and the access through it is guarded with that reason.
#include <stdint.h>
#include <weavec.h>

void poke(volatile unsigned *WEAVEC_RAW reg) {
  *reg = 1; // BUG: unsafe-operation definite
}

void poke_address(uintptr_t addr) {
  volatile unsigned *mmio = (volatile unsigned *)addr; // UNRESOLVED: spatial:raw-cast // UNRESOLVED: temporal:raw-cast
  *mmio = 1; // UNRESOLVED: spatial:raw-cast // UNRESOLVED: temporal:raw-cast // GUARDED: spatial
}
