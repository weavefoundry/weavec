// RFC 0053 §3 (D2): a callable of any type that is kept (a binder, a functor, a
// factory's result, stored in a `std::function` member) holds no loan but of
// owners; one returned holds none of the function's locals.
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

// lifetimes/k03_kept_bind
namespace k03_kept_bind {
struct Bus { std::function<void()> f; void fire() const { f(); } };
void set(V &v) { v = K; }
int hole() {
  V v = std::string("hello");
  Bus b;
  b.f = std::bind(set, std::ref(v)); // expected-error {{'std::bind(set, std::ref(v))' is kept by 'b.f' and holds a borrow of 'v' [weavec-safe:escape]}}
  std::string &s = std::get<std::string>(v);
  b.fire();
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace k03_kept_bind

// lifetimes/k04_kept_bindfront
namespace k04_kept_bindfront {
struct Bus { std::function<void()> f; void fire() const { f(); } };
void set(V &v) { v = K; }
int hole() {
  V v = std::string("hello");
  Bus b;
  b.f = std::bind_front(set, std::ref(v)); // expected-error {{'std::bind_front(set, std::ref(v))' is kept by 'b.f' and holds a borrow of 'v' [weavec-safe:escape]}}
  std::string &s = std::get<std::string>(v);
  b.fire();
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace k04_kept_bindfront

// lifetimes/k07_kept_factory
namespace k07_kept_factory {
struct Bus { std::function<void()> f; void fire() const { f(); } };
auto mk(V &v) { return [&v] { v = K; }; }
int hole() {
  V v = std::string("hello");
  Bus b;
  b.f = mk(v); // expected-error {{'mk(v)' is kept by 'b.f' and holds a borrow of 'v' [weavec-safe:escape]}}
  std::string &s = std::get<std::string>(v);
  b.fire();
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace k07_kept_factory

// lifetimes/k08_kept_memfn
namespace k08_kept_memfn {
struct Bus { std::function<void()> f; void fire() const { f(); } };
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  Bus b;
  M m{v}; b.f = std::bind_front(&M::flip, m); // expected-error {{'std::bind_front(&M::flip, m)' is kept by 'b.f' and holds a borrow of 'v' [weavec-safe:escape]}}
  std::string &s = std::get<std::string>(v);
  b.fire();
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace k08_kept_memfn

// lifetimes/k12_this_bindfront
namespace k12_this_bindfront {
struct C {
  V v = std::string("hello");
  std::function<void()> cb;
  void flip() { v = K; }
  void setup() { cb = std::bind_front(&C::flip, this); } // expected-error {{'std::bind_front(&C::flip, this)' may change 'this object', which is passed with it [weavec-safe:call-alias]}} expected-error {{'std::bind_front(&C::flip, this)' is kept by 'this->cb' and holds a borrow of 'this object' [weavec-safe:escape]}}
  void run() { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }
};
int hole() { C c; c.setup(); c.run(); std::printf("long now %lx\n", std::get<long>(c.v)); }
} // namespace k12_this_bindfront

// types/t36a_functor_ref_function
namespace t36a_functor_ref_function {
struct Flip { V &v; void operator()() const { v = K; } };
int hole() {
  V v = std::string(48, 'x');
  std::function<void()> f = Flip{v};
  const std::string &s = std::get<std::string>(v);
  f();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t36a_functor_ref_function

// types/t36b_functor_kept
namespace t36b_functor_kept {
struct Flip { V &v; void operator()() const { v = K; } };
struct Holder { std::function<void()> f; };
int hole() {
  V v = std::string(48, 'x');
  Holder h;
  h.f = Flip{v}; // expected-error {{'Flip{v}' is kept by 'h.f' and holds a borrow of 'v' [weavec-safe:escape]}}
  const std::string &s = std::get<std::string>(v);
  h.f();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'h.f', which may change what 'h.f' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t36b_functor_kept

// lifetimes/v19_ret_fn_bind
namespace v19_ret_fn_bind {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
__attribute__((noinline)) std::function<long()> mk() { long x = 5; return std::bind_front([](long *y) { return *y; }, &x); } // expected-error {{what is returned points into 'x', which does not outlive the function [weavec-safe:escape]}}
int hole() {
  auto r = mk(); smash(); std::printf("%lx\n", r());
}
} // namespace v19_ret_fn_bind
