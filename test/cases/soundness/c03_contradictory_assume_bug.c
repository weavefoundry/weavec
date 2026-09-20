// ASAN
#include <weavec.h>
int main(void) {
  char b[4] = {0};
  int i = 10;
  WEAVEC_ASSUME(i < 4); // BUG: contradicted-assumption definite
  return b[i];
}
