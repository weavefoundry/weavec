// RFC 0030 §4 "Assumptions" (c): an assumption the engine refutes is an error.
// STAGE: S3
// "assumption 'i < 4' is false here [weavec::contradicted-assumption]". The analysis still
// assumes 'i < 4' after the site, so b[i] reports nothing more. ASan (without WeaveC, where
// the macro compiles to nothing) confirms the out-of-bounds read.
// ASAN
#include <weavec.h>

int c(void) {
  char b[4] = {0};
  int i = 10;
  WEAVEC_ASSUME(i < 4); // BUG: contradicted-assumption definite
  return b[i];
}

int main(void) { return c(); }
