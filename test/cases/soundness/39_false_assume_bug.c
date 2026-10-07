// ASAN
#include <stdlib.h>
#include <weavec.h>
int main(int argc, char **argv) {
  (void)argv;
  char buf[4] = {0};
  int i = argc + 10;
  WEAVEC_ASSUME(i < 4); // BUG: contradicted-assumption // MISS: the analysis does not refute an assumption on a value from the input
  return buf[i]; // TRAP
}
