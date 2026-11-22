// RFC 0052 §3 (D3): a reference or pointer a call returns points into every
// argument whose type reaches it, a member function's into its object too;
// `lifetimebound` narrows. The round of 2026-11-21's `lifetimes/p54`,
// `p55`, `p55b`, `p56`, `p57`, `p59`, `p74`, `p77` and `types/p10`.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <any>
#include <map>
#include <string>
#include <variant>
#include <weavec.h>

using V = std::variant<std::string, long>;
constexpr long K = 0x1111111111111111L;

// p54, p55, p55b: into an argument of a type that holds the result
std::string &first(V &b) { return std::get<std::string>(b); }
std::string &pick(V &, V &b) { return std::get<std::string>(b); }
void fromHelper() {
  V a = std::string("a"), b = std::string("hello");
  std::string &r = first(b);
  b = K;
  r[0] = 'Z'; // expected-error {{'r' points into 'b', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::string &q = pick(a, b);
  a = K;
  q[0] = 'Z'; // expected-error {{'q' points into 'a', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// `lifetimebound` says which.
std::string &picked(V &, V &b WEAVEC_LIFETIMEBOUND) {
  return std::get<std::string>(b);
}
void narrowed() {
  V a = std::string("a"), b = std::string("hello");
  std::string &q = picked(a, b);
  a = K;
  q[0] = 'Z';
}
// p56: a member function's result, into an argument as well
struct Getter {
  int n = 0;
  std::string &get(V &other) { return std::get<std::string>(other); }
};
void memberResult() {
  V v = std::string("hello");
  Getter g;
  std::string &r = g.get(v);
  v = K;
  r[0] = 'Z'; // expected-error {{'r' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// ... but not into a key whose type cannot hold it.
int found(const std::map<std::string, int> &m) {
  auto it = m.find(std::string("k"));
  return it == m.end() ? 0 : it->second;
}
// p57: where a pointer member of an argument points
struct Box {
  V *p;
};
std::string &inside(const Box &b) { return std::get<std::string>(*b.p); }
void throughBox() {
  V v = std::string("hello");
  Box b{&v};
  std::string &r = inside(b);
  v = K;
  r[0] = 'Z'; // expected-error {{'r' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// p74: a value whose pointer member points into an argument
struct Ref {
  std::string *p;
};
Ref grab(V &v) { return Ref{&std::get<std::string>(v)}; }
void valueResult() {
  V v = std::string("hello");
  Ref r = grab(v);
  v = K;
  (*r.p)[0] = 'Z'; // expected-error {{'r' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// p59, p77: a character of a string
char &at(std::string &s, unsigned long i) { return s[i]; }
void grown() {
  std::string s = "abcdefgh";
  char &c = at(s, 0);
  s.append(100, 'g');
  c = 'X'; // expected-error {{'c' points into 's', which 'append' may have changed [weavec-safe:stale-borrow]}}
  const char *p = &at(s, 1);
  s.append(100, 'g');
  (void)*p; // expected-error {{'p' points into 's', which 'append' may have changed [weavec-safe:stale-borrow]}}
}
// types/p10: into what `std::any` holds
void anyCast() {
  std::any a = 5L;
  long *p = std::any_cast<long>(&a);
  a = std::string(48, 'x');
  (void)*p; // expected-error {{'p' points into 'a', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// A reference to the argument itself survives a change of it.
const std::string &same(const std::string &a, const std::string &b) {
  return a.size() > b.size() ? a : b;
}
void whole(std::string &x, std::string &y) {
  const std::string &r = same(x, y);
  y.append("z");
  (void)r.size();
}
