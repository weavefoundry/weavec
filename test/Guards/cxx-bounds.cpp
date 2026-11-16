// RFC 0047 §1.4: a safe C++ function's accesses through a pointer with a
// bound are compared with the bound (`pointer-out-of-bounds`): a member's
// count, a parameter's, a member function's result counted by a member,
// `main`'s argv; a guarded caller that overstates a count stops at the
// entry; an end pointer one past its array is not checked as an object.
// RUN: %weavec_cxx -std=c++20 -O2 -g -fweavec-safe %s -o %t
// RUN: %t
// RUN: not --crash %t member 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not --crash %t result 2>&1 | FileCheck %s --check-prefix=RESULT
// RUN: not --crash %t entry 2>&1 | FileCheck %s --check-prefix=ENTRY
// RUN: not --crash %t argv 2>&1 | FileCheck %s --check-prefix=ARGV
// RUN: %weavec_cxx -std=c++20 -O0 -g -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: %t.full
// RUN: not --crash %t.full member 2>&1 | FileCheck %s --check-prefix=MEMBER
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <weavec.h>

class Slice {
public:
  Slice(const char *WEAVEC_COUNTED_BY(n) WEAVEC_ESCAPES d, std::size_t n) : data_(d), size_(n) {}
  // MEMBER: weavec: pointer-out-of-bounds at {{.*}}cxx-bounds.cpp:[[@LINE+2]]:{{[0-9]+}}: read of 1 bytes at 0x{{[0-9a-f]+}}
  // MEMBER: weavec: 0x{{[0-9a-f]+}} is 2 bytes after the 4-byte bounds of the pointer at 0x{{[0-9a-f]+}}
  char operator[](std::size_t i) const { return data_[i]; }
  const char *WEAVEC_COUNTED_BY(size_) data() const { return data_; }
  void remove_prefix(std::size_t n) {
    data_ += n;
    size_ -= n;
  }

private:
  const char *WEAVEC_COUNTED_BY(size_) data_;
  std::size_t size_;
};

// RESULT: weavec: pointer-out-of-bounds at {{.*}}cxx-bounds.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 1 bytes
char peek(const Slice &s, std::size_t i) { return s.data()[i]; }

unsigned decode(const char *WEAVEC_ENDED_BY(limit) p, const char *limit) {
  unsigned r = 0;
  while (p < limit)
    r = (r * 10) + unsigned(*p++ - '0');
  return r;
}

// ENTRY: weavec: global-buffer-overflow at {{.*}}cxx-bounds.cpp:{{[0-9]+}}:{{([0-9]+:)?}} read of 100 bytes
WEAVEC_GUARDED char lie() {
  static const char text[] = "abcdef";
  Slice s(text, 100);
  return s[0];
}

int main(int argc, char **argv) {
  char text[6] = {'1', '2', '3', '4', '5', '6'};
  Slice s(text, 6);
  s.remove_prefix(2);
  const char *mode = argc > 1 ? argv[1] : "";
  if (std::strcmp(mode, "member") == 0)
    return s[6];
  if (std::strcmp(mode, "result") == 0)
    return peek(s, 4);
  if (std::strcmp(mode, "entry") == 0)
    return lie();
  // ARGV: weavec: pointer-out-of-bounds at {{.*}}cxx-bounds.cpp:[[@LINE+2]]:{{[0-9]+}}: read of 8 bytes
  if (std::strcmp(mode, "argv") == 0)
    return argv[argc][0];
  std::printf("%c %c %u\n", s[0], peek(s, 3), decode(text, text + 6));
  return 0;
}
