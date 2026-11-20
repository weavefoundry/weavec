// RFC 0050 §2.3: a view a fast safe function makes from a pointer and a
// count is guarded once, as a range, where it is made, so that the view's
// trusted accesses need none: a view one too long stops at its making, not
// at the first element past the pointer's object. One that fits runs.
// RUN: %weavec_cxx -std=c++20 -O1 -g -fweavec-safe %s -o %t
// RUN: %t fits | FileCheck %s --check-prefix=FITS
// RUN: not --crash %t 2>&1 | FileCheck %s
// RUN: not --crash %t span 2>&1 | FileCheck %s --check-prefix=SPAN
// A count whose bytes overflow (here, to none) stops too.
// RUN: not --crash %t wrap 2>&1 | FileCheck %s --check-prefix=WRAP
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>
#include <weavec.h>

[[gnu::noinline]] std::string_view tail(const char *WEAVEC_COUNTED_BY(n) p,
                                        std::size_t n, std::size_t more) {
  // CHECK: weavec: heap-buffer-overflow at {{.*}}view-made.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  return std::string_view(p, n + more);
}

[[gnu::noinline]] std::span<const long> longs(const long *WEAVEC_COUNTED_BY(n) p,
                                              std::size_t n) {
  // SPAN: weavec: heap-buffer-overflow at {{.*}}view-made.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 80 bytes
  return std::span<const long>(p, n + 2);
}

[[gnu::noinline]] std::span<const long>
wrapped(const long *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  // WRAP: weavec: heap-buffer-overflow at {{.*}}view-made.cpp:[[@LINE+1]]:{{[0-9]+}}: read of
  return std::span<const long>(p, (n / 8) << 61);
}

[[gnu::noinline]] char last(std::string_view view) {
  return view.empty() ? '\0' : view.back();
}

WEAVEC_GUARDED bool asked(int argc, char **argv, const char *mode) {
  return argc > 1 && std::strcmp(argv[1], mode) == 0;
}

int main(int argc, char **argv) {
  std::vector<char> text(8, 'a');
  std::vector<long> numbers(8, 1);
  if (asked(argc, argv, "span"))
    return static_cast<int>(longs(numbers.data(), numbers.size()).back());
  if (asked(argc, argv, "wrap"))
    return static_cast<int>(wrapped(numbers.data(), numbers.size())[9]);
  // FITS: a
  std::printf("%c\n", last(tail(text.data(), text.size(),
                                asked(argc, argv, "fits") ? 0 : 1)));
  return 0;
}
