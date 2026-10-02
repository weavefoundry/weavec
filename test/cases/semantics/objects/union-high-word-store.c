// RFC 0031 *Implementation amendments* (stores past the caller's object): a
// store into part of a scalar.
// STAGE: S7
// `clear_sign` writes the high 32-bit word of a union that also holds a
// `double` (dtoa's `word0(d) &= 0x7fffffff`). Its summary names that word
// as a byte offset into the union's first member, the `double`; the caller
// must not read the store as a whole `double` starting 4 bytes in, which
// would end 4 bytes past the 8-byte union and be a definite (and false)
// `out-of-bounds` at the call.
// CLEAN
// ASAN
// RUN-INPUT:
#include <stdint.h>

typedef union {
  double d;
  uint32_t L[2];
} U;

static void clear_sign(U *u) { u->L[1] &= 0x7fffffffu; }

int main(void) {
  U u;
  u.d = -2.0;
  clear_sign(&u);
  return u.d == 2.0 ? 0 : 1;
}
