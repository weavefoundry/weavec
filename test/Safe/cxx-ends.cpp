// RFC 0048 §2.2 (D2): a `T *` in safe C++ points to one object. A position
// computed by arithmetic from a bounded pointer is not returned, stored or
// passed where a plain pointer is declared (`call-args`): an end says
// WEAVEC_COUNTED_BY(0), a cursor WEAVEC_ENDED_BY. An end is never read
// (`index-single`). A C library function does not access, through a pointer
// to one object, more than the object's bytes (`call-args`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -w -DGUARDED %s
#include <cstddef>
#include <cstring>
#include <weavec.h>
#ifdef GUARDED
#define CHECKED WEAVEC_GUARDED
#else
#define CHECKED
#endif

class Slice {
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
  const char *mark_ = nullptr;

public:
  CHECKED const char *end() const {
    return data_ + size_; // expected-error {{'data_ + size_' is a position, returned where a pointer to one object is declared [weavec-safe:call-args]}}
  }
  CHECKED const char *WEAVEC_COUNTED_BY(0) limit() const { return data_ + size_; }
  CHECKED const char *at(std::size_t i) const { return &data_[i]; }
  CHECKED void markEnd() {
    mark_ = data_ + size_; // expected-error {{'data_ + size_' is a position, stored in 'mark_' where a pointer to one object is declared [weavec-safe:call-args]}}
  }
  CHECKED char past() const {
    return *limit(); // expected-error {{'limit()' points to no element and is read [weavec-safe:index-single]}}
  }
};

CHECKED int one(const char *p);
CHECKED int walk(const char *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  const char *q = p;
  q += n;
  return one(q); // expected-error {{'q' is a position, passed for 'p' of 'one' where a pointer to one object is declared [weavec-safe:call-args]}}
}

// The end a WEAVEC_ENDED_BY names is no object.
CHECKED int count(const char *WEAVEC_ENDED_BY(end) begin, const char *end);
CHECKED int counted(const char *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  return count(p, p + n);
}

CHECKED void copy(char *WEAVEC_COUNTED_BY(n) out, const char *in, std::size_t n) {
  std::memcpy(out, in, n); // expected-error {{'in' points to one object, and 'memcpy' may access more than its 1 byte through it [weavec-safe:call-args]}}
}
CHECKED void copyOne(char *out, const char *in) { std::memcpy(out, in, 1); }
CHECKED bool equal(const char *WEAVEC_COUNTED_BY(n) a, std::size_t n, const char *b) {
  return std::strncmp(a, b, n) == 0; // expected-error {{'b' points to one object, and 'strncmp' may access more than its 1 byte through it [weavec-safe:call-args]}}
}
CHECKED bool equalStrings(const char *WEAVEC_NULL_TERMINATED a,
                          const char *WEAVEC_NULL_TERMINATED b, std::size_t n) {
  return std::strncmp(a, b, n) == 0;
}
