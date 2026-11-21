// RFC 0052 §6 (D7): a value whose members could hold a borrow of what it was
// built from is a view of it, raw pointer members included; a list is a view
// of the array its braces made; a shared pointer the aliasing constructor
// makes is a view of what its second argument points into. None of a local
// is returned. The round of 2026-11-21's `lifetimes/p17b`, `p20`, `p42`,
// `p45` and `types/p25`, `p26`.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <initializer_list>
#include <memory>
#include <ranges>
#include <string>
#include <vector>
#include <weavec.h>

// p42: a struct with a pointer member
struct R {
  long *p;
};
R made() {
  long x = 5;
  return R{&x}; // expected-error {{'R{&x}' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
}
R passedOn(long &x) { return R{&x}; }
// A member borrows only what its type could point into: the sink, not the
// prefix it copies.
struct Sink {
  std::vector<std::string> lines;
};
struct Logger {
  std::string prefix;
  Sink *sink;
  Logger(const std::string &p, Sink &WEAVEC_ESCAPES s) : prefix(p), sink(&s) {}
};
Logger logger(Sink &WEAVEC_ESCAPES sink) {
  std::string prefix = "log: ";
  return Logger(prefix, sink);
}
// p20, types/p25: the aliasing constructor
std::shared_ptr<long> aliased(const std::shared_ptr<int> &owner) {
  long local = 0x1111;
  return std::shared_ptr<long>(owner, &local); // expected-error {{'std::shared_ptr<long>(owner, &local)' holds a borrow of 'local' and is returned [weavec-safe:escape]}}
}
struct Pair {
  long first = 0;
  long second = 0;
};
std::shared_ptr<long> member(const std::shared_ptr<Pair> &owner) {
  return std::shared_ptr<long>(owner, &owner->second);
}
// p17b, types/p26: a returned list
std::initializer_list<long> listed(long a) {
  return {a, a + 1, a + 2}; // expected-error {{the 'std::initializer_list' that is returned points into the array its braces made, which does not outlive the function [weavec-safe:escape]}}
}
std::initializer_list<std::string> names() {
  return {std::string("one"), std::string("two")}; // expected-error {{the 'std::initializer_list' that is returned points into the array its braces made, which does not outlive the function [weavec-safe:escape]}}
}
long sum(std::initializer_list<long> values) {
  long total = 0;
  for (long value : values)
    total += value;
  return total;
}
long summed() { return sum({1, 2, 3}); }
// p45: a range adaptor of a local
auto all() {
  std::vector<long> v{1, 2, 3};
  return std::views::all(v); // expected-error {{'std::views::all(v)' holds a borrow of 'v' and is returned [weavec-safe:escape]}}
}
auto allOf(std::vector<long> &v) { return std::views::all(v); }
std::vector<long> evens(const std::vector<long> &v) {
  std::vector<long> out;
  for (long x : v | std::views::filter([](long n) { return n % 2 == 0; }))
    out.push_back(x);
  return out;
}
