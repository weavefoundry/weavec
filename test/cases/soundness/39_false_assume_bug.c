// ASAN
#include <stdlib.h>
#include <weavec.h>
int main(int argc, char **argv) {
  (void)argv;
  char buf[4] = {0};
  int i = argc + 10;
  WEAVEC_ASSUME(i < 4); // BUG: contradicted-assumption // TRAP: assert
  return buf[i]; // TRAP: index
}
