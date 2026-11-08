// RFC 0039 §9, C++ detection: a live caller's local overflowed after an exception was caught. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <stdexcept>
#include <string>
__attribute__((noinline)) static int risky(int x) {
  std::string s(64, 'k');
  if (x > 0)
    throw std::runtime_error(s);
  return 0;
}
__attribute__((noinline)) static void fill(char *p, int n) {
  for (int i = 0; i < n; ++i)
    p[i] = 'x'; // STOP
}
int main(int argc, char **) {
  char buffer[16];
  try {
    (void)risky(argc);
  } catch (const std::exception &) {
  }
#ifdef FIX
  fill(buffer, 16);
#else
  fill(buffer, 40);
#endif
  return buffer[0] == 'x' ? 0 : 1;
}
