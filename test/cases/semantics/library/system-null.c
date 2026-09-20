// RFC 0030 §8.3: system(NULL) is allowed.
// STAGE: S4
// The row is 'system (r:str:null-ok)': a null command asks whether a shell exists, so the
// null argument is neither an error nor a check. No error, no trap.
// CLEAN
// ASAN
#include <stdlib.h>

int main(void) { return system(NULL) ? 0 : 1; }
