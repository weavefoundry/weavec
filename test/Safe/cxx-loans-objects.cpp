// RFC 0053 §6, §7 (D5, D6, D7): a polymorphic object holds the loans of what it
// was made from, and a virtual call reaches them; a value is returned with a
// borrow of a parameter only if its type shows it or the parameter is
// `lifetimebound`; a borrow into a mutable global ends at a call of the
// program, and none is returned into one; `mutable` makes `const` shallow for
// a caller; `= default` is no owner's destructor.
// RFC 0052's G4 round (`build/rfc52/g4`): its holes and the programs its
// checked build stops, each in a namespace named after it.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <algorithm>
#include <any>
#include <array>
#include <deque>
#include <exception>
#include <list>
#include <map>
#include <ranges>
#include <set>
#include <stack>
#include <unordered_map>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include <weavec.h>

using V = std::variant<std::string, long>;
#define K 0x1111111111111111L

// types/t08_virtual_ref_member
namespace t08_virtual_ref_member {
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &x) : v(x) {} void run() override { v = K; } };
static void go(Base &b) { b.run(); }
int hole() {
  V v = std::string(48, 'x');
  D d(v);
  const std::string &s = std::get<std::string>(v);
  go(d);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into 'v', which the call to 'go' may reach through 'd' [weavec-safe:stale-borrow]}}
}
} // namespace t08_virtual_ref_member

// types/t09_virtual_unique
namespace t09_virtual_unique {
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &x) : v(x) {} void run() override { v = K; } };
static std::unique_ptr<Base> make(V &v) { return std::make_unique<D>(v); } // expected-error {{what is returned holds a borrow of 'v', which its type 'std::unique_ptr<Base>' does not show [weavec-safe:escape]}}
int hole() {
  V v = std::string(48, 'x');
  std::unique_ptr<Base> b = make(v);
  const std::string &s = std::get<std::string>(v);
  b->run();
  std::cout << "s = " << s << "\n";
}
} // namespace t09_virtual_unique

// types/t33d_unique_refmember
namespace t33d_unique_refmember {
struct Flip { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string(48, 'x');
  auto p = std::make_unique<Flip>(v);
  const std::string &s = std::get<std::string>(v);
  p->flip();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into 'v', which the call to 'flip' may reach through its object [weavec-safe:stale-borrow]}}
}
} // namespace t33d_unique_refmember

// types/t06_thread_local_global
namespace t06_thread_local_global {
thread_local V g = std::string(48, 'x');
static void flip() { g = K; }
int hole() {
  const std::string &s = std::get<std::string>(g);
  flip();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to 'flip' may change [weavec-safe:stale-borrow]}}
}
} // namespace t06_thread_local_global

// types/t07_static_local_getter
namespace t07_static_local_getter {
static V &get() { thread_local V v = std::string(48, 'x'); return v; }
int hole() {
  const std::string &s = std::get<std::string>(get());
  get() = K;
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into a global, which the call to 'get' may change [weavec-safe:stale-borrow]}}
}
} // namespace t07_static_local_getter

// lifetimes/q01_tls_global
namespace q01_tls_global {
thread_local V g = std::string("hello");
void flip() { g = K; }
int hole() {
  std::string &s = std::get<std::string>(g);
  flip();
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to 'flip' may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace q01_tls_global

// lifetimes/q02_tls_result
namespace q02_tls_result {
thread_local V g = std::string("hello");
std::string &gs() { return std::get<std::string>(g); } // expected-error {{what is returned points into the global 'g', which a call of the program may change [weavec-safe:escape]}}
int hole() {
  std::string &s = gs();
  g = K;
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace q02_tls_result

// types/t11_mutable_member
namespace t11_mutable_member {
struct Box { mutable V v; };
static void f(const Box &b) { b.v = K; }
int hole() {
  Box box{std::string(48, 'x')};
  const std::string &s = std::get<std::string>(box.v);
  f(box);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'box' before the call to 'f', which may change what 'box' owns [weavec-safe:stale-borrow]}}
}
} // namespace t11_mutable_member

// types/t11b_mutable_any
namespace t11b_mutable_any {
struct Box { mutable std::any a; };
static void f(const Box &b) { b.a = K; }
int hole() {
  Box box{std::string(48, 'x')};
  const std::string &s = std::any_cast<const std::string &>(box.a);
  f(box);
  std::cout << "s.c_str() = " << s.c_str() << " size=" << s.size() << std::endl; // expected-error {{'s' was taken from 'box' before the call to 'f', which may change what 'box' owns [weavec-safe:stale-borrow]}}
}
} // namespace t11b_mutable_any

// types/t33b_defaulted_dtor_return
namespace t33b_defaulted_dtor_return {
struct Own { long *p; ~Own() = default; };
static Own make() { long x = 0x1111; return Own{&x}; } // expected-error {{'Own{&x}' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
static long other() { long y[8] = {7, 7, 7, 7, 7, 7, 7, 7}; long t = 0; for (long e : y) t += e; return t; }
int hole() {
  Own o = make();
  long t = other();
  std::cout << std::hex << "*p = " << *o.p << " (expected 1111), t=" << t << "\n";
}
} // namespace t33b_defaulted_dtor_return
