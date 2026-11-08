// RFC 0039 §9, C++ detection: ostream::write of a buffer past its block. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <sstream>
#include <cstdlib>
#include <cstring>
int main() {
  char *bytes = static_cast<char *>(malloc(8));
  memset(bytes, 'b', 8);
  std::ostringstream out;
#ifdef FIX
  out.write(bytes, 8);
#else
  out.write(bytes, 12); // STOP
#endif
  return out.str().size() == 8 ? 0 : 1;
}
