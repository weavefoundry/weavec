// RFC 0049: the second unit of test/Driver/safe-fix.cpp.
#include "fix-buffer.h"

std::size_t count(const char *text, std::size_t n, char c) {
  std::size_t hits = 0;
  for (std::size_t i = 0; i < n; ++i)
    if (text[i] == c)
      ++hits;
  return hits;
}
