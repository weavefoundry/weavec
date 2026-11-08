// RFC 0039 §9, C++ detection: a pointer into a frame an exception unwound. -DFIX is the correct twin.
// DETECT: -DFIX
// FLAGS: -O1
// RUN-INPUT: x
#include <stdexcept>
static int *escaped;
static int saved[4];
__attribute__((noinline)) static void thrower(int n) {
  int local[4] = {n, 0, 0, 0};
#ifdef FIX
  escaped = saved;
  saved[0] = n;
#else
  escaped = local;
#endif
  if (n > 0)
    throw std::runtime_error("no");
  (void)local;
}
int main(int argc, char **) {
  try {
    thrower(argc);
  } catch (const std::exception &) {
  }
  return escaped[0] == 1 ? 0 : 1; // STOP
}
