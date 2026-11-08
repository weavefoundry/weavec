// RFC 0039 §9, C++ detection: an iterator past a vector's end, within its capacity. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <vector>
int main(int argc, char **) {
  std::vector<int> v;
  v.reserve(16);
  for (int i = 0; i < 4; ++i)
    v.push_back(i);
#ifdef FIX
  v.push_back(4);
#endif
  auto it = v.begin() + 2 + argc;
  return *it == 4 ? 0 : 1; // STOP
}
