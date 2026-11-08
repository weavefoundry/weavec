// RFC 0039 §9, C++ detection: vector::operator[] past the size (the C++ library's own check). -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <vector>
int main(int argc, char **) {
  std::vector<int> v(4);
#ifdef FIX
  v.resize(8);
#endif
  return v[static_cast<unsigned>(argc) + 3]; // STOP
}
