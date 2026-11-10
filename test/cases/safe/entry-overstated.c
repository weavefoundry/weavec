// RFC 0041 §5, amendment 18: a guarded caller that overstates a count stops
// when the safe function is entered; the validating entry is inlined, so the
// report names the call.
// FLAGS: -O1
#include <weavec.h>
#include <stddef.h>

WEAVEC_SAFE int first(const int *WEAVEC_COUNTED_BY(n) v, size_t n) {
  return n ? v[0] : 0;
}

int main(int argc, char **argv) {
  (void)argv;
  int a[4] = {1, 2, 3, 4};
  a[3] = argc; // a stays a local (a constant array becomes untracked data)
  return first(a, 64); // TRAP: stack-buffer-overflow
}
