// RFC 0039 §9, C++ detection: a read past a deque's elements in its block. -DFIX is the correct twin.
// libstdc++ annotates no deque (RFC 0039, amendment 10).
// REQUIRES: darwin
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <deque>
int main() {
  std::deque<int> d;
  for (int i = 0; i < 4; ++i)
    d.push_back(i);
#ifdef FIX
  d.push_back(4);
#endif
  const int *at = &d[0];
  return at[4] == 4 ? 0 : 1; // STOP
}
