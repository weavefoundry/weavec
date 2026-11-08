// RFC 0039 §9, C++ detection: std::string::find of a buffer past its block. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <string>
#include <cstdlib>
#include <cstring>
int main() {
  char *bytes = static_cast<char *>(malloc(8));
  memset(bytes, 'b', 8);
  std::string s(32, 'b');
#ifdef FIX
  return static_cast<int>(s.find(bytes, 0, 8));
#else
  return static_cast<int>(s.find(bytes, 0, 12)); // STOP
#endif
}
