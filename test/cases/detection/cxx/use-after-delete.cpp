// RFC 0039 §9, C++ detection: use after delete. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <memory>
static int *volatile kept;
int main() {
  int *p = new int(7);
  kept = p;
#ifndef FIX
  delete p;
#endif
  int v = *kept; // STOP
#ifdef FIX
  delete p;
#endif
  return v == 7 ? 0 : 1;
}
