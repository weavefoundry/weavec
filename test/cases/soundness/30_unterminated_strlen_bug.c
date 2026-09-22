// strlen on a non-terminated array.
// ASAN
#include <string.h>
int main(void) {
  char buf[4] = {'a', 'b', 'c', 'd'};
  return (int)strlen(buf); // BUG: out-of-bounds
}
