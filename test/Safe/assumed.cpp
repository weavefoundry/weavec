// RFC 0051 §4.1: what only asserts is not accepted in a safe function:
// `[[assume]]`, the assume builtins, an asserting attribute on the function
// or on a function it calls that the unit's own files declare, and a loop's
// `assume_safety`. Each error's fix (certain) deletes it, or writes the
// form that asserts nothing. Hints the optimizer may act on only as hints
// are accepted.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// RUN: not %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -fno-caret-diagnostics %s 2>&1 | FileCheck %s
#include <array>
#include <cstddef>
#include <vector>

long at(const std::array<long, 8> &cells, std::size_t i) {
  [[assume(i < 8)]]; // expected-error {{'assume' tells the optimizer what nothing checks}}
  return cells[i];
}
// CHECK: assumed.cpp:[[@LINE-3]]:5: error: 'assume' tells the optimizer what nothing checks [weavec-safe:assumed]
// CHECK: note: fix (certain): remove '{{\[\[}}assume(i < 8)]];'

long hinted(const std::vector<long> &v, std::size_t i) {
  __builtin_assume(i < v.size()); // expected-error {{'__builtin_assume' tells the optimizer what nothing checks}}
  return v[i];
}

const long *aligned(const std::vector<long> &v) {
  return static_cast<const long *>(__builtin_assume_aligned(v.data(), 64)); // expected-error {{'__builtin_assume_aligned' tells the optimizer what nothing checks}}
}
// CHECK: note: fix (certain): write 'v.data()' for '__builtin_assume_aligned(v.data(), 64)'

[[gnu::pure]] long count(const std::vector<long> &v); // expected-error {{'pure' tells the optimizer what nothing checks}}
long counted(const std::vector<long> &v) { return count(v) + count(v); }
// CHECK: note: fix (certain): remove '{{\[\[}}gnu::pure]] '

[[gnu::returns_nonnull]] const long *first(const std::vector<long> &v) { // expected-error {{'returns_nonnull' tells the optimizer what nothing checks}}
  return v.empty() ? nullptr : &v[0];
}

long looped(std::vector<long> &v) {
  long sum = 0;
  // expected-error@+1 {{'assume_safety' tells the optimizer what nothing checks}}
#pragma clang loop vectorize(assume_safety)
  for (std::size_t i = 0; i + 1 < v.size(); ++i)
    sum += v[i];
  return sum;
}
// CHECK: note: fix (certain): write 'enable' for 'assume_safety'

// Accepted: hints that are only hints, and promises the compiler checks.
long likely(long x) {
  if (x > 0) [[likely]]
    return x;
  return -x;
}
long expected(long x) { return __builtin_expect(x, 0); }
[[nodiscard]] long kept(long x) noexcept { return x; }
long count(const std::vector<long> &v) { return static_cast<long>(v.size()); }
long vectorized(std::vector<long> &v) {
  long sum = 0;
#pragma clang loop vectorize(enable)
  for (long x : v)
    sum += x;
  return sum;
}
