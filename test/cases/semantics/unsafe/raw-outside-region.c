// RFC 0030 §6.1 and §2.3: outside a region a Raw site is an unsafe-operation error and an IntToPtr site is raw-cast.
// STAGE: S3
// 'reg' is converted from an integer, an RFC 0004 raw origin. Outside an unsafe region the
// IntToPtr site's own facets are unresolved(raw-cast), and the dereference of the raw
// pointer is the unsafe-operation error of RFC 0004, as before.
#include <stdint.h>

void poke(uintptr_t addr) {
  volatile unsigned *reg = (volatile unsigned *)addr; // UNRESOLVED: spatial:raw-cast // UNRESOLVED: temporal:raw-cast
  *reg = 1; // BUG: unsafe-operation definite
}
