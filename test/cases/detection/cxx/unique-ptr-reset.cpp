// RFC 0039 §9, C++ detection: an object used after its unique_ptr is reset. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <memory>
struct Box { int x[4] = {1, 2, 3, 4}; };
int main() {
  auto owner = std::make_unique<Box>();
  Box *raw = owner.get();
#ifndef FIX
  owner.reset();
#endif
  return raw->x[0] == 1 ? 0 : 1; // STOP
}
