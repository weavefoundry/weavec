// RFC 0034 §4: parameters whose addresses escape are registered like locals,
// on a granule, so two of them side by side keep their own shadow: a write
// through either pointer stays inside its parameter, and leaving one does
// not leave the other's bytes behind for a later frame (an alloca'd buffer
// below) to trip over.
// STAGE: S4
// CLEAN
// RUN-INPUT:
// ASAN
#include <alloca.h>
#include <string.h>
typedef struct { long v; } Value;
static __attribute__((noinline)) int step(Value *x, Value *y, int i) {
  x->v += i;
  y->v += i;
  return i < 3;
}
static __attribute__((noinline)) long compare(Value x, Value y) {
  int i = 0;
  while (step(&x, &y, i))
    ++i;
  return x.v + y.v;
}
static __attribute__((noinline)) long scratch(int n) {
  long *buffer = alloca(sizeof(long) * (size_t)n);
  long sum = 0;
  for (int i = 0; i < n; i++)
    buffer[i] = i;
  for (int i = 0; i < n; i++)
    sum += buffer[i];
  return sum;
}
int main(void) {
  Value a = {1}, b = {2};
  long total = 0;
  for (int round = 0; round < 3; round++)
    total += compare(a, b) + scratch(24);
  return total == 3 * (15 + 276) ? 0 : 1;
}
