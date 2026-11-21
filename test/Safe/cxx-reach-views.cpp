// RFC 0052 §4 (D4): `const` on a view is shallow. A view of the program's
// class reaches what it points to at a call of its member functions; a
// change through a reference wrapper's result or a span's element is a
// change of the object it names; a view made at an object of its own type
// points at it. The round of 2026-11-21's `lifetimes/p13`, `p31` and
// `types/p18`, `p19`.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <functional>
#include <span>
#include <string>
#include <variant>

using V = std::variant<std::string, long>;
constexpr long K = 0x1111111111111111L;

// p13: a reference member's object, reached through its holder
struct M {
  V &v;
  void flip() { v = K; }
  long size() const { return static_cast<long>(v.index()); }
};
void holder() {
  V v = std::string("hello");
  M m{v};
  std::string &s = std::get<std::string>(v);
  m.flip();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'flip' may reach through its object [weavec-safe:stale-borrow]}}
}
void flips(M m) { m.flip(); }
void passedByValue() {
  V v = std::string("hello");
  M m{v};
  std::string &s = std::get<std::string>(v);
  flips(m);
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'flips' may reach through 'm' [weavec-safe:stale-borrow]}}
}
// p31, types/p18: through `std::reference_wrapper`
void wrapped() {
  V v = std::string("hello");
  auto w = std::ref(v);
  std::string &r = std::get<std::string>(v);
  w.get() = K;
  r[0] = 'Z'; // expected-error {{'r' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// types/p19: through a span's element
void spanned() {
  V arr[1] = {V(std::string(48, 'x'))};
  std::span<V> s1(arr);
  const std::string &s = std::get<0>(arr[0]);
  s1[0] = K;
  (void)s.size(); // expected-error {{'s' points into 'arr', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
void spannedParameter(std::span<V> s1) {
  const std::string &s = std::get<0>(s1[0]);
  s1[0] = K;
  (void)s.size(); // expected-error {{'s' points into 's1', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// A view of `const` reaches nothing a call could change.
long counts(std::span<const V> s) { return static_cast<long>(s.size()); }
void readOnly() {
  V arr[1] = {V(std::string(48, 'x'))};
  const std::string &s = std::get<0>(arr[0]);
  (void)counts(arr);
  (void)s.size();
}
// A reference to the object itself survives a change of its value.
void stillThere() {
  V v = std::string("hello");
  auto w = std::ref(v);
  v = K;
  (void)w.get().index();
}
