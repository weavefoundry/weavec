// RFC 0030 §7.4 (a cursor's span check): a pointer that may point into one
// of several arrays of one size has no single base to check a span against.
// Checking the cursor against the first array's span traps when it walks the
// second; the access is guarded instead.
// CLEAN
// RUN-INPUT:
#include <string.h>
int main(void) {
  const char *first[] = {"1", "2", "3"};
  const char *second[] = {"a", "b", "c"};
  const char **all[] = {first, second};
  int total = 0;
  for (unsigned s = 0; s < 2; ++s) {
    const char **cursor = all[s];
    for (unsigned i = 0; i < 3; ++i) {
      total += (int)strlen(*cursor);
      cursor++;
    }
  }
  return total == 6 ? 0 : 1;
}
