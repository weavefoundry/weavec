// RFC 0047 §1: Safe C's bounds in safe C++: a pointer with a bound is
// indexed and stepped, arrays and containers' data() have bounds, a member's
// count is paired with its pointer, and the library's pointer-and-count
// parameters ask for a bound (`index-single`, `call-args`, `counted-pair`,
// `field-address`, `bad-annotation`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// A guarded unit keeps compiling whatever its classes' bounds say.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -w -DGUARDED %s
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <weavec.h>

class Slice {
public:
  Slice() = default;
  Slice(const char *WEAVEC_COUNTED_BY(n) d, std::size_t n) : data_(d), size_(n) {}
  char operator[](std::size_t i) const { return data_[i]; }
  const char *WEAVEC_COUNTED_BY(size_) data() const { return data_; }
  const char *end() const { return data_ + size_; }
  std::size_t size() const { return size_; }
  void remove_prefix(std::size_t n) {
    data_ += n;
    size_ -= n;
  }
  void resize(std::size_t n) {
    size_ = n; // expected-error {{'size_' bounds 'data_' and is assigned without it [weavec-safe:counted-pair]}}
  }
  void swapSize(Slice &o) {
    std::swap(size_, o.size_); // expected-error 2 {{a reference to 'size_', which bounds a pointer, would let one of the two change without the other [weavec-safe:field-address]}}
  }
  void swap(Slice &o) { std::swap(*this, o); }
  std::size_t &count() {
    std::size_t &n = size_; // expected-error {{a reference to 'size_', which bounds a pointer, would let one of the two change without the other [weavec-safe:field-address]}}
    return n;
  }
  std::string str() const { return std::string(data_, size_); }

private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};

#ifndef GUARDED
struct Misnamed {
  char *WEAVEC_COUNTED_BY(len + 1) p; // expected-error {{'len + 1' in the bound of field 'p' names no field of the struct [weavec-safe:bad-annotation]}}
  std::size_t len;
};
#endif
class Emptied {
public:
  Emptied() : size_(0) {}
  explicit Emptied(int) : size_{} {}

private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = "";
  std::size_t size_;
};

class Unpaired {
public:
  Unpaired(const char *WEAVEC_COUNTED_BY(n) d, std::size_t n) : size_(n) { (void)d; } // expected-error {{'size_' bounds 'data_' and is initialized without it [weavec-safe:counted-pair]}}

private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};

template <class T> struct Buffer {
  T *WEAVEC_COUNTED_BY(n) p = nullptr;
  std::size_t n = 0;
  T at(std::size_t i) const { return p[i]; }
};
int instantiated(const Buffer<int> &b) { return b.n != 0 ? b.at(0) : 0; }

int single(const int *p) {
  return p[1]; // expected-error {{'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]}}
}
int sum(const std::vector<int> &v) {
  int s = 0;
  const int *p = v.data();
  for (std::size_t i = 0; i < v.size(); ++i)
    s += p[i];
  return s;
}
unsigned decode(const char *WEAVEC_ENDED_BY(limit) p, const char *limit) {
  unsigned r = 0;
  while (p < limit)
    r = (r * 10) + unsigned(*p++ - '0');
  return r;
}
std::size_t length(const char *WEAVEC_NULL_TERMINATED s) { return std::strlen(s); }
std::size_t ofString(const std::string &s) { return length(s.c_str()); }
int arrays(int i) {
  char buf[8] = {};
  std::memset(buf, 1, sizeof buf);
  const char *p = buf;
  std::array<int, 4> a{1, 2, 3, 4};
  int t = 0;
  for (auto it = a.begin(); it != a.end(); ++it)
    t += *it;
  return p[i & 7] + t;
}
int captured(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  auto f = [p] { return p[1]; }; // expected-error {{'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]}}
  return n > 1 ? f() + p[1] : 0;
}
int unbounded(const int *p) {
  std::vector<int> v(p, p + 1); // expected-error {{argument 1 of 'vector' must have bounds, and this pointer points to one object [weavec-safe:call-args]}} expected-error {{'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]}}
  return sum(v);
}
std::string_view view(const char *p, std::size_t n) {
  return std::string_view(p, n); // expected-error {{argument 1 of 'basic_string_view' must have bounds, and this pointer points to one object [weavec-safe:call-args]}}
}
std::span<const int> spanned(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  return std::span<const int>(p, n);
}
void appended(std::string &out, const char *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  out.append(p, n);
}
int nulled() { return length(nullptr) != 0; } // expected-error {{null passed for 's' of 'length', which points to a string [weavec-safe:call-args]}}
int called(const char *p) {
  return int(decode(p, p)); // expected-error {{the argument for 'p' of 'decode' must have bounds, and this pointer points to one object [weavec-safe:call-args]}}
}

// bad-annotation: declarations and overrides agree.
int counted(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n);
int counted(const int *p, std::size_t n) { // expected-error {{this declaration of 'counted' gives its pointers other bounds than an earlier one [weavec-safe:bad-annotation]}}
  return n != 0 ? *p : 0;
}
struct Reader {
  virtual ~Reader() = default;
  virtual std::size_t read(char *WEAVEC_COUNTED_BY(n) to, std::size_t n) = 0;
};
struct Zeros : Reader {
  std::size_t read(char *to, std::size_t n) override { // expected-error {{'read' overrides 'Reader::read' with other bounds [weavec-safe:bad-annotation]}}
    (void)to;
    return n;
  }
};
void owned(int *WEAVEC_OWNED p) { // expected-error {{'weavec.owned' is Safe C's, and a safe C++ function does not use it [weavec-safe:bad-annotation]}}
  (void)p;
}

int main(int argc, char **argv) {
  const char text[] = "123456";
  Slice s(text, 6);
  s.remove_prefix(2);
  return s[0] + (argc > 1 ? argv[1][0] : 0) + int(decode(text, text + 6));
}
