// RFC 0049: a unit that includes weavec.h before fix-pair.h, for
// test/Driver/safe-fix-include.cpp.
#include <weavec.h>
#include "fix-pair.h"

int first() {
  int v[2] = {1, 2};
  return second(v);
}
