// RFC 0039 §9, C++ detection: new[] released by delete. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <cstddef>
struct Item { int x = 0; };
static Item *volatile kept;
int main() {
  Item *items = new Item[4];
  kept = items;
#ifdef FIX
  delete[] kept;
#else
  delete kept; // STOP
#endif
  return 0;
}
