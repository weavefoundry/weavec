// RFC 0039 §9, C++ detection: basic_string::copy past its destination (checked at its first byte only). -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <string>
#include <cstdlib>
int main() {
  char *bytes = static_cast<char *>(malloc(8));
  std::string s(32, 'c');
#ifdef FIX
  size_t n = s.copy(bytes, 8);
#else
  size_t n = s.copy(bytes, 16); // STOP // MISS: copy is checked at its first byte (RFC 0039, Bugs deliberately not caught)
#endif
  return n == 8 ? 0 : 1;
}
