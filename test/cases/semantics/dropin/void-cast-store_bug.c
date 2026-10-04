// RFC 0033 §1: the twin of void-cast-store_ok.c. The callee overwrites the
// caller's valid pointer with a null read through a `void *`; the caller's
// use after the call is not proven from the value it held before.
// STAGE: S8
// RUN-INPUT: 1
#include <stdlib.h>
#include <string.h>
typedef struct val { unsigned long tag; const char *str; } val;
static const char *get_str(void *v) { return ((val *)v)->str; }
static __attribute__((noinline)) void take(val *v, const char **value) { *value = get_str(v); }
int main(int argc, char **argv) {
  (void)argv;
  val v = {5, argc > 5 ? "x" : NULL};
  const char *s = "valid";
  take(&v, &s);
  return (int)strlen(s); // BUG: null-dereference possible // TRAP: nonnull
}
