// RFC 0039 §9, C++ detection: a pointer kept across a vector reallocation. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <vector>
int main(int argc, char **) {
  std::vector<int> v{1, 2, 3};
#ifdef FIX
  v.reserve(400);
#endif
  int *first = &v[0];
  for (int i = 0; i < 100 * argc; ++i)
    v.push_back(i);
  return *first == 1 ? 0 : 1; // STOP
}
