// RFC 0039 §9, C++ detection: delete through a base without a virtual destructor. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <cstddef>
struct Base { int base[4]; };
struct Derived : Base { int more[8]; };
#ifdef FIX
struct VBase { virtual ~VBase() = default; int base[4]; };
struct VDerived : VBase { int more[8]; };
static VBase *volatile kept;
int main() {
  kept = new VDerived;
  delete kept;
  return 0;
}
#else
static Base *volatile kept;
int main() {
  kept = new Derived;
  delete kept; // STOP
  return 0;
}
#endif
