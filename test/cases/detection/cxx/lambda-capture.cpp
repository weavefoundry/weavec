// RFC 0039 §9, C++ detection: a lambda that captured a local by reference, called after its scope. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <functional>
static std::function<int()> later;
__attribute__((noinline)) static void arm(int n) {
#ifdef FIX
  later = [n] { return n; };
#else
  int local = n;
  later = [&local] { return local; }; // STOP
#endif
}
int main(int argc, char **) {
  arm(argc);
  return later() == 1 ? 0 : 1; // STOP
}
