// pointer-bytes in C++: a pointer is made only from what it points to.
// Bytes are not written into an object that holds a pointer (a library
// call's written argument, `std::bit_cast` or `__builtin_bit_cast` to it):
// an integer made of a pointer and turned back into one would point
// anywhere. A whole object zeroed, or copied from its own type, is
// accepted, and so is a pointer read as an integer.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <span>
#include <string_view>

struct Ref {
  const long *p = nullptr;
  long n = 0;
};

Ref forge(const long *p) {
  return std::bit_cast<Ref>(std::array<long, 2>{std::bit_cast<long>(p) + 32, 1}); // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
}

void overwrite(Ref &r, long bits) {
  std::memcpy(&r, &bits, sizeof bits); // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
}

bool load(std::FILE *file, Ref &r) {
  return std::fread(&r, sizeof r, 1, file) == 1; // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
}

long deref(std::uintptr_t bits) {
  const long *p = std::bit_cast<const long *>(bits); // expected-error {{bytes are written into a pointer, which they would forge}}
  return *p;
}

long first(std::array<long, 2> raw) {
  const auto view = std::bit_cast<std::span<long>>(raw); // expected-error {{bytes are written into a 'std::span<long>', which holds a pointer they would forge}}
  return view[0];
}

char letter(std::array<long, 2> raw) {
  const auto text = __builtin_bit_cast(std::string_view, raw); // expected-error {{bytes are written into a 'std::string_view', which holds a pointer they would forge}} expected-error {{makes or reads a pointer}}
  return text[0];
}

// Accepted: the whole object zeroed, a copy of its own type, a pointer read
// as an integer, bytes written into an object without a pointer, and an
// object the library writes as its own (a pointer it stores through an out
// parameter, a `tm`, a `FILE`), whose size the caller does not give.
void clear(Ref &r) { std::memset(&r, 0, sizeof r); }
void copy(Ref &to, const Ref &from) { std::memcpy(&to, &from, sizeof to); }
std::uintptr_t address(const long *p) { return std::bit_cast<std::uintptr_t>(p); }
double real(std::uint64_t bits) { return std::bit_cast<double>(bits); }
void fill(std::array<long, 2> &a, const std::array<unsigned char, 16> &bytes) {
  std::memcpy(a.data(), bytes.data(), sizeof a);
}
double number(const char *text) {
  char *end = nullptr;
  return std::strtod(text, &end);
}
int hour(std::time_t when) {
  std::tm parts{};
  ::localtime_r(&when, &parts);
  return parts.tm_hour;
}
void flush(std::FILE *file) { std::fflush(file); }
