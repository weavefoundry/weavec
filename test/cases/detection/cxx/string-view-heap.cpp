// RFC 0039 §9, C++ detection: a string_view of a returned long string. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <string>
#include <string_view>
__attribute__((noinline)) static std::string make() { return std::string(100, 'z'); }
int main() {
#ifdef FIX
  std::string owner = make();
  std::string_view s = owner;
#else
  std::string_view s = make();
#endif
  return s[3] == 'z' ? 0 : 1; // STOP
}
