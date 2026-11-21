// RFC 0051 §4.4 (D5): in a unit compiled with -fweavec-safe, four of
// Clang's warnings are errors, with Clang's own words and names: a path
// that returns no value, a `[[noreturn]]` function that returns, and a
// format that does not match its arguments or is not a literal.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// RUN: not %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -fno-caret-diagnostics %s 2>&1 | FileCheck %s
// Without the flag they stay warnings.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -Wno-everything %s
#include <cstdio>
#include <stdexcept>
#include <string>

int sign(int x) {
  if (x > 0)
    return 1;
  if (x < 0)
    return -1;
} // expected-error {{non-void function does not return a value in all control paths}}
// CHECK: defined.cpp:[[@LINE-1]]:1: error: non-void function does not return a value in all control paths [-Werror,-Wreturn-type]

[[noreturn]] void fail(const std::string &what, bool fatal) {
  if (fatal)
    throw std::runtime_error(what);
} // expected-error {{function declared 'noreturn' should not return}}
// CHECK: error: function declared 'noreturn' should not return [-Werror,-Winvalid-noreturn]

void show(const std::string &text, long n) {
  std::printf(text.c_str()); // expected-error {{format string is not a string literal (potentially insecure)}}
  std::printf("%s\n", n); // expected-error {{format specifies type 'char *' but the argument has type 'long'}}
}
// CHECK: [-Werror,-Wformat-security]
// CHECK: [-Werror,-Wformat]

// Accepted: an exhaustive switch over an enumeration (its end stops when
// it is reached; Clang does not warn), a format that matches.
enum class Op { Add, Sub };
long step(Op op, long a, long b) {
  switch (op) {
  case Op::Add:
    return a + b;
  case Op::Sub:
    return a - b;
  }
}
void print(long n) { std::printf("%ld\n", n); }
