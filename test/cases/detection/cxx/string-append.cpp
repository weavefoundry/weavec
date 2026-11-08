// RFC 0039 §9, C++ detection: std::string::append from a buffer past its block (the C++ library's shared object). -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <string>
#include <cstdlib>
#include <cstring>
int main() {
  char *bytes = static_cast<char *>(malloc(8));
  memset(bytes, 'b', 8);
  std::string s;
#ifdef FIX
  s.append(bytes, 8);
#else
  s.append(bytes, 12); // STOP
#endif
  return s.size() == 8 ? 0 : 1;
}
