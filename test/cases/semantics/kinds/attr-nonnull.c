// RFC 0030 §7.2 (nonnull, _Nonnull): ecosystem nullability attributes in user code are declared requirements.
// STAGE: S6
// In non-system code 'nonnull(1)' and '_Nonnull' make the parameter non-null (precedence
// level 2): each call wraps a possibly-null argument in the nonnull check. The first run
// passes a null pointer to 'get', the second to 'get2'.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include <stddef.h>
#include <stdlib.h>

int get(const int *p) __attribute__((nonnull(1)));
int get(const int *p) { return *p; }

int get2(const int *_Nonnull p) { return *p; }

int main(int argc, char **argv) {
  static const int x = 7;
  int which = argc > 1 ? atoi(argv[1]) : 0;
  const int *p = which == 0 ? &x : NULL;
  if (which == 1) return get(p); // TRAP: nonnull
  return get2(p); // TRAP: nonnull
}
