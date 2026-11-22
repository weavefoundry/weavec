// RFC 0053 §8: a function that marks a parameter `lifetimebound` returns a
// borrow only of what it marks; a member function of a borrowing class
// returns where its object points; what a callable returns points where its
// loans reach.
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

// lifetimes/r02_ptrstruct_member
namespace r02_ptrstruct_member {
struct P { V *p; std::string &s() const { return std::get<std::string>(*p); } };
int hole() {
  V v = std::string("hello");
  P pp{&v}; std::string &s = pp.s();
  v = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r02_ptrstruct_member

// lifetimes/r03_lifetimebound_wrong
namespace r03_lifetimebound_wrong {
std::string &pick(V &a [[clang::lifetimebound]], V &b) { (void)a; return std::get<std::string>(b); } // expected-error {{what is returned points into 'b', which is not marked WEAVEC_LIFETIMEBOUND [weavec-safe:escape]}}
int hole() {
  V v = std::string("hello");
  V other = 2L; std::string &s = pick(other, v);
  v = K;
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r03_lifetimebound_wrong

// lifetimes/r03b_lifetimebound_tmp
namespace r03b_lifetimebound_tmp {
const std::string &pick(const std::string &a [[clang::lifetimebound]], const std::string &b) { (void)a; return b; } // expected-error {{what is returned points into 'b', which is not marked WEAVEC_LIFETIMEBOUND [weavec-safe:escape]}}
int hole() {
  std::string keep = "kept";
  const std::string &r = pick(keep, std::string(40, 'T'));
  std::string other(40, 'Q');
  std::printf("%s\n", r.c_str());
}
} // namespace r03b_lifetimebound_tmp

// lifetimes/r04_fn_result
namespace r04_fn_result {
void dummy() {}
int hole() {
  V v = std::string("hello");
  std::function<std::string &()> f = [&v]() -> std::string & { return std::get<std::string>(v); }; std::string &s = f();
  v = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r04_fn_result

// lifetimes/w13_fn_ref_result
namespace w13_fn_ref_result {
int hole() {
  V v = std::string("hello");
  std::function<V &()> f = [&v]() -> V & { return v; };
  std::string &s = std::get<std::string>(v);
  f() = K;
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which changes 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace w13_fn_ref_result

// lifetimes/v20_ret_memfn
namespace v20_ret_memfn {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
struct L { long x = 5; long get() const { return x; } };
__attribute__((noinline)) auto mk() { L l; return std::bind_front(&L::get, &l); } // expected-error {{'std::bind_front(&L::get, &l)' holds a borrow of 'l' and is returned [weavec-safe:escape]}}
int hole() {
  auto r = mk(); smash(); std::printf("%lx\n", r());
}
} // namespace v20_ret_memfn

// lifetimes/v20b_ret_memfn_write
namespace v20b_ret_memfn_write {
struct L { long x = 5; void set() { x = 0x4444444444444444L; } };
__attribute__((noinline)) auto mk() { L l; return std::bind_front(&L::set, &l); } // expected-error {{'std::bind_front(&L::set, &l)' holds a borrow of 'l' and is returned [weavec-safe:escape]}}
template <class F> __attribute__((noinline)) void victim(F &f) { volatile long a[8] = {1, 1, 1, 1, 1, 1, 1, 1}; f(); long m = 0; for (int i = 0; i < 8; i++) m |= a[i]; std::printf("victim locals OR (should be 1): %lx\n", m); }
int hole() { auto r = mk(); victim(r); }
} // namespace v20b_ret_memfn_write

// lifetimes/d03_dead_memfn
namespace d03_dead_memfn {
struct Bus { std::function<void()> f; };
void setl(long &x) { x = 0x4141414141414141L; }
auto mk(long &x) { return [&x] { x = 0x4141414141414141L; }; }
struct L { long x = 5; void flip() { x = 0x4141414141414141L; } };
__attribute__((noinline)) long probe() { volatile long a[16]; for (int i = 0; i < 16; i++) a[i] = 0; long m = 0; for (int i = 0; i < 16; i++) m |= a[i]; return m; }
__attribute__((noinline)) long probe2() { volatile long a[16]; long m = 0; for (int i = 0; i < 16; i++) m |= a[i]; return m; }
__attribute__((noinline)) void reg(Bus &b) {
  L l; b.f = std::bind_front(&L::flip, &l); std::printf("x %ld\n", l.x); // expected-error {{'std::bind_front(&L::flip, &l)' is kept by 'b.f' and holds a borrow of 'l' [weavec-safe:escape]}}
}
int hole() { Bus b; reg(b); probe(); b.f(); std::printf("dead frame read %lx\n", probe2()); }
} // namespace d03_dead_memfn

// lifetimes/d03b_dead_memfn_reuse
namespace d03b_dead_memfn_reuse {
struct Bus { std::function<void()> f; };
struct L { long x = 5; void flip() { x = 0x4141414141414141L; } };
__attribute__((noinline)) void reg(Bus &b) { L l; b.f = std::bind_front(&L::flip, &l); std::printf("x %ld\n", l.x); } // expected-error {{'std::bind_front(&L::flip, &l)' is kept by 'b.f' and holds a borrow of 'l' [weavec-safe:escape]}}
int hole() {
  Bus b;
  reg(b);
  auto other = std::make_unique<L>();
  b.f();
  std::printf("other L, never written: %lx\n", other->x);
}
} // namespace d03b_dead_memfn_reuse

// lifetimes/d03c_dead_memfn_frame
namespace d03c_dead_memfn_frame {
struct Bus { std::function<void()> f; };
struct L { long x = 5; void flip() { x = 0x4141414141414141L; } };
__attribute__((noinline)) void reg(Bus &b) { L l; b.f = std::bind_front(&L::flip, &l); std::printf("x %ld\n", l.x); } // expected-error {{'std::bind_front(&L::flip, &l)' is kept by 'b.f' and holds a borrow of 'l' [weavec-safe:escape]}}
// Same frame shape as reg; y is never written by this function after construction.
__attribute__((noinline)) void victim(Bus &b) { L y; Bus mine; mine.f = std::bind_front(&L::flip, &y); b.f(); std::printf("victim's y.x (should be 5): %lx\n", y.x); } // expected-error {{'std::bind_front(&L::flip, &y)' is kept by 'mine.f' and holds a borrow of 'y' [weavec-safe:escape]}}
int hole() {
  Bus b;
  reg(b);
  victim(b);
}
} // namespace d03c_dead_memfn_frame
