// RFC 0039 §9, C++ detection: an object used after its last shared_ptr is gone. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <memory>
struct Box { int x[4] = {1, 2, 3, 4}; };
int main() {
  Box *raw;
  std::shared_ptr<Box> keep;
  {
    auto owner = std::make_shared<Box>();
    raw = owner.get();
#ifdef FIX
    keep = owner;
#endif
  }
  return raw->x[1] == 2 ? 0 : 1; // STOP
}
