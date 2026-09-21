// CLEAN
// The filling loop's body is three statements, and the contiguous-fill
// recogniser of §7.4 takes only a loop whose whole body is the one store, so
// the cells are tracked one at a time through the symbolic place
// 'items[array-index(i)]'. That place outlives the loop with its counter now
// past the end of the array, so the release range [0, 4) of either cleanup
// loop does not contain it: the concrete cells are released, the summary
// place is not, and both exits report it as a possible leak. Recognising a
// fill loop that bails out on a failed allocation would retire it; that is an
// engine change RFC 0030 has not made, so the warning is tolerated here.
// ALLOW: leak
// ASAN
#include <stdlib.h>
int main(void) {
  char *items[4] = {0};
  for (int i = 0; i < 4; i++) { items[i] = malloc(4); if (!items[i]) goto out; items[i][0] = (char)i; }
  int r = items[2][0];
  for (int i = 0; i < 4; i++) free(items[i]);
  return r;
out:
  for (int i = 0; i < 4; i++) free(items[i]);
  return 1;
}
