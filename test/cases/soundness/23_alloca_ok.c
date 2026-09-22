// CLEAN
// ASAN
#include <alloca.h>
int main(void) {
  char *q = alloca(8);
  q[7] = 1;
  return q[7];
}
