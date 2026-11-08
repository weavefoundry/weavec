// RFC 0039 §9, C++ detection: istream::read into a buffer past its block. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <sstream>
#include <cstdlib>
#include <string>
int main() {
  char *bytes = static_cast<char *>(malloc(8));
  std::istringstream in(std::string(64, 'i'));
#ifdef FIX
  in.read(bytes, 8);
#else
  in.read(bytes, 12); // STOP
#endif
  return bytes[0] == 'i' ? 0 : 1;
}
