// RFC 0039 §9, C++ detection: a string_view of a returned short string (its inline buffer). -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <string>
#include <string_view>
__attribute__((noinline)) static std::string make() { return std::string(3, 'y'); }
int main() {
  std::string_view s;
#ifdef FIX
  std::string owner = make();
  s = owner;
#else
  s = make();
#endif
  return s[1] == 'y' ? 0 : 1; // STOP
}
