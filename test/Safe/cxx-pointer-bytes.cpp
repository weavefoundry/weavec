// pointer-bytes in C++: a pointer is made only from what it points to.
// Bytes are not written into an object that holds a pointer (a library
// call's written argument, `std::bit_cast` or `__builtin_bit_cast` to it):
// an integer made of a pointer and turned back into one would point
// anywhere. The object is what the argument points to before it becomes
// the call's `void *`: an array's element (an array passed by name), and
// through a cast to `void *`. A whole object zeroed (a row's `fills`), its
// elements reordered (`permutes`), or copied from its own type, is
// accepted, and so is a pointer read as an integer. A library call that
// writes pointers of its own making into a caller's array (`backtrace`) is
// not.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#define __STDC_WANT_LIB_EXT1__ 1
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <string.h>
#include <strings.h>
#include <span>
#include <string_view>
#include <vector>

struct Ref {
  const long *p = nullptr;
  long n = 0;
};

Ref forge(const long *p) {
  // RFC 0052 §6: and 'Ref''s pointer could point into the temporary.
  return std::bit_cast<Ref>(std::array<long, 2>{std::bit_cast<long>(p) + 32, 1}); // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}} expected-error {{the value holds a borrow of a temporary 'std::array<long, 2>' and is returned [weavec-safe:escape]}}
}

void overwrite(Ref &r, long bits) {
  std::memcpy(&r, &bits, sizeof bits); // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
}

bool load(std::FILE *file, Ref &r) {
  return std::fread(&r, sizeof r, 1, file) == 1; // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
}

long overwriteArray(long bits) {
  Ref refs[1];
  std::memcpy(refs, &bits, sizeof bits); // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
  return refs[0].n;
}

bool overwritePointers(long bits) {
  const long *pointers[2] = {};
  std::memcpy(pointers, &bits, sizeof bits); // expected-error {{bytes are written into a pointer, which they would forge}}
  return pointers[1] == nullptr;
}

void overwriteThroughVoid(Ref &r, long bits) {
  std::memcpy(static_cast<void *>(&r), &bits, sizeof bits); // expected-error {{bytes are written into a 'Ref', which holds a pointer ('p') they would forge}}
}

int frames() {
  void *at[8];
  return ::backtrace(at, 8); // expected-error {{bytes are written into a pointer, which they would forge}}
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
void clearAll(Ref (&refs)[2]) { std::memset(refs, 0, sizeof refs); }
void zero(Ref &r) { ::bzero(&r, sizeof r); }
void zeroArray() {
  Ref refs[2];
  ::bzero(refs, sizeof refs);
}
#if defined(__APPLE__)
void wipe(Ref &r) { ::memset_s(&r, sizeof r, 0, sizeof r); }
#else
void wipe(Ref &r) { ::explicit_bzero(&r, sizeof r); }
#endif
static int byName(const void *, const void *) { return 0; }
void sort(std::vector<const char *> &names) {
  std::qsort(names.data(), names.size(), sizeof(const char *), byName);
}
void sortArray() {
  const char *names[3] = {"b", "a", "c"};
  std::qsort(names, 3, sizeof names[0], byName);
}
void copy(Ref &to, const Ref &from) { std::memcpy(&to, &from, sizeof to); }
void copyArray(Ref (&to)[2], const Ref (&from)[2]) {
  std::memcpy(to, from, sizeof to);
}
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
