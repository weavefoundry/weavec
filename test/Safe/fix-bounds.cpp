// RFC 0049 §2, §3.2: an error that wants a bound carries a fix, from what
// the uses of the declaration show inside the functions they are in: a
// library call that pairs the pointer with a count (E1), a comparison that
// bounds its index (E2), a byte tested against zero (E4), constant indices
// (E5), a bound that flows in or out (E6). The note says the text to write
// at the declaration and carries it as a fix-it. Uses that disagree make a
// choice, which carries none; an end is WEAVEC_COUNTED_BY(0) (C3), and a
// constructor's initializer is checked as an assignment is (§4).
// RUN: not %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s
#include <cstddef>
#include <cstring>
#include <string>
#include <weavec.h>

// E1 (a library call), E6 (a result returns a bounded member).
class Slice {
public:
  Slice(const char *WEAVEC_COUNTED_BY(n) WEAVEC_ESCAPES d, std::size_t n)
      : data_(d), size_(n) {}
  const char *data() const { return data_; }
  std::size_t size() const { return size_; }
  bool starts(const Slice &x) const {
    return size_ >= x.size_ && std::memcmp(data_, x.data_, x.size_) == 0;
  }
  std::string text() const { return std::string(data_, size_); }

private:
  const char *WEAVEC_BORROWED data_ = "";
  std::size_t size_ = 0;
};
// CHECK: error: 'data_' points to one object, and 'memcmp' may access more than its 1 byte through it [weavec-safe:call-args]
// CHECK: fix-bounds.cpp:28:15: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(size_) WEAVEC_BORROWED data_'; 'size_' is its count in the call to 'memcmp' at line 23
// CHECK: fix-it:"{{.*}}fix-bounds.cpp":{28:15-28:15}:"WEAVEC_COUNTED_BY(size_) "
// CHECK: error: argument 1 of 'basic_string' must have bounds, and this pointer points to one object [weavec-safe:call-args]
// CHECK: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(size_) WEAVEC_BORROWED data_'

// E2: the loop compares the index with `n`.
std::size_t count(const char *text, std::size_t n, char c) {
  std::size_t hits = 0;
  for (std::size_t i = 0; i < n; ++i)
    hits += text[i] == c ? 1 : 0;
  return hits;
}
// CHECK: fix-bounds.cpp:{{[0-9]+}}:13: error: 'text' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(n) text'; 'n' bounds its index at line {{[0-9]+}}
// CHECK: fix-it:"{{.*}}fix-bounds.cpp":{{[{][0-9]+}}:31-{{[0-9]+}}:31}:"WEAVEC_COUNTED_BY(n) "

// E5: constant indices only.
int pair(const int *p) { return p[0] + p[1]; }
// CHECK: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: fix (likely): declare it 'const int *WEAVEC_COUNTED_BY(2) p'; only its elements 0 to 1 are read

// E4: a byte tested against zero, and the pointer against null.
std::size_t length(const char *s) {
  if (s == nullptr)
    return 0;
  std::size_t n = 0;
  while (s[n] != '\0')
    ++n;
  return n;
}
// CHECK: error: 's' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: fix (likely): declare it 'const char *WEAVEC_NULL_TERMINATED_OR_NULL s'; a byte of it is tested against zero at line {{[0-9]+}}

// Uses that disagree: a choice, with no fix-it.
int either(const char *p, std::size_t n) {
  int sum = 0;
  for (std::size_t i = 0; i < n; ++i)
    sum += p[i];
  return p[n] == '\0' ? sum : -sum;
}
// CHECK: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: choice: 'p' is WEAVEC_COUNTED_BY(n) ('n' bounds its index at line {{[0-9]+}}), or WEAVEC_NULL_TERMINATED (a byte of it is tested against zero at line {{[0-9]+}})
// CHECK-NOT: fix-it
// CHECK: error: 'p' points to one object

// C3: the end of its own bound.
class Span {
  const int *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;

public:
  const int *end() const { return data_ + size_; }
};
// CHECK: error: 'data_ + size_' is a position, returned where a pointer to one object is declared [weavec-safe:call-args]
// CHECK: note: fix (certain): declare it 'const int *WEAVEC_COUNTED_BY(0) end'
// CHECK: fix-it:"{{.*}}fix-bounds.cpp":{{[{][0-9]+}}:14-{{[0-9]+}}:14}:"WEAVEC_COUNTED_BY(0) "

// §4: a constructor's initializer, checked as an assignment is; E6 names
// the count set beside it.
class Owned {
  const char *WEAVEC_COUNTED_BY(size_) data_;
  std::size_t size_;

public:
  Owned(const char *WEAVEC_ESCAPES d, std::size_t n) : data_(d), size_(n) {}
};
// CHECK: error: 'd' points to one object, and field 'data_' has a bound [weavec-safe:call-args]
// CHECK: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(n) WEAVEC_ESCAPES d'; 'size_' is set from 'n' beside it

// CHECK: note: 10 of 10 errors carry a fix; 'weavec-c++ --weavec-fix -- <this command>' applies them
