// RFC 0031 §4.9, *Summaries*: the elements of a member array.
// STAGE: S7
// `divide` rewrites the bytes of the array member of one record (bzip2's
// `uInt64_qrm10`). Its summary names them as `param0->b[*]`, eight elements of
// one byte; naming them as elements of an array of records (`param0[*].b`)
// made the caller's check read eight records of eight bytes behind a single
// local, a definite (and false) `out-of-bounds` at the call.
// CLEAN
// ASAN
// RUN-INPUT:
typedef struct {
  unsigned char b[8];
} UInt64;

static int divide(UInt64 *n) {
  unsigned rem = 0;
  for (int i = 7; i >= 0; i--) {
    unsigned tmp = rem * 256 + n->b[i];
    n->b[i] = (unsigned char)(tmp / 10);
    rem = tmp % 10;
  }
  return (int)rem;
}

int main(void) {
  UInt64 n = {{0}};
  n.b[0] = 123;
  UInt64 copy = n;
  int digit = divide(&copy);
  return digit == 3 && copy.b[0] == 12 ? 0 : 1;
}
