// RFC 0030 §7.2 (WEAVEC_NONNULL on a parameter): a possibly-null argument is checked at the call.
// STAGE: S6
// The declaration makes 'p' non-null: the dereference inside 'get' is proven, and a call
// whose argument has unknown nullness wraps it in the nonnull check instead of reporting it
// (*Annotation surface*). The run passes a null pointer.
// RUN-INPUT:
// ASAN
#include <stddef.h>
#include <weavec.h>

int get(const int *WEAVEC_NONNULL p) { return *p; }

int main(int argc, char **argv) {
  static const int x = 7;
  (void)argv;
  return get(argc > 1 ? &x : NULL); // BUG: null-dereference // TRAP: nonnull
}
