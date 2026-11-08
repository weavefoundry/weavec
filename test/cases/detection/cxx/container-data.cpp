// RFC 0039 §9, C++ detection: a read past a vector's elements through data(), within its capacity. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <vector>
int main() {
  std::vector<int> v;
  v.reserve(8);
  v.push_back(1);
#ifdef FIX
  v.resize(6);
#endif
  return v.data()[5]; // STOP
}
