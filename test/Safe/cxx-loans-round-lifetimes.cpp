// RFC 0053's own G4 round (`build/rfc53/g4/lifetimes`), the holes the compiler that
// went to it accepted and its amendments 13 to 19 reject: a pair or tuple
// assigned or swapped under C++23's `const` twins, a `std::any` read by value,
// a view of a view, a range adaptor that holds a lambda, a destructor that
// reaches what its object holds, calls of the program the first build missed.
// Each in a namespace named after it.
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

// lifetimes/a14_any_ptr
namespace a14_any_ptr {
int hole() {
  V v = std::string("hello");
  std::any a = &v;
  std::string &s = std::get<std::string>(v);
  *std::any_cast<V *>(a) = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace a14_any_ptr

// lifetimes/a24_structured
namespace a24_structured {
int hole() {
  std::pair<V, int> p{std::string("hello"), 1};
  auto &[a, b] = p;
  std::string &s = std::get<std::string>(a);
  p = {V(K), 2};
  s[0] = 'Z'; // expected-error {{'s' points into 'p', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(p.first));
}
} // namespace a24_structured

// lifetimes/b04_bind_tuple
namespace b04_bind_tuple {
int hole() {
  std::tuple<V, int> t{std::string("hello"), 1};
  auto &[a, b] = t;
  std::string &s = std::get<std::string>(a);
  t = std::tuple<V, int>{V(K), 2};
  s[0] = 'Z'; // expected-error {{'s' points into 't', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(std::get<0>(t)));
}
} // namespace b04_bind_tuple

// lifetimes/b11_tuple_ref_assign
namespace b11_tuple_ref_assign {
int hole() {
  V v = std::string("hello");
  std::tuple<V &> t{v};
  std::string &s = std::get<std::string>(v);
  t = std::tuple<V>(V(K));
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace b11_tuple_ref_assign

// lifetimes/b12_pair_ref_assign
namespace b12_pair_ref_assign {
int hole() {
  V v = std::string("hello");
  int n = 0;
  std::pair<V &, int &> p{v, n};
  std::string &s = std::get<std::string>(v);
  p = std::pair<V, int>(V(K), 1);
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace b12_pair_ref_assign

// lifetimes/b14_any_refwrap
namespace b14_any_refwrap {
int hole() {
  V v = std::string("hello");
  std::any a = std::ref(v);
  std::string &s = std::get<std::string>(v);
  std::any_cast<std::reference_wrapper<V>>(a).get() = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace b14_any_refwrap

// lifetimes/c01_pair_assign
namespace c01_pair_assign {
int hole() {
  std::pair<V, int> p{std::string("hello"), 1};
  std::string &s = std::get<std::string>(p.first);
  p = {V(K), 2};
  s[0] = 'Z'; // expected-error {{'s' points into 'p', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(p.first));
}
} // namespace c01_pair_assign

// lifetimes/c02_pair_assign2
namespace c02_pair_assign2 {
int hole() {
  std::pair<V, int> p{std::string("hello"), 1};
  std::string &s = std::get<std::string>(p.first);
  p = std::pair<V, int>(V(K), 2);
  s[0] = 'Z'; // expected-error {{'s' points into 'p', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(p.first));
}
} // namespace c02_pair_assign2

// lifetimes/c03_tuple_assign
namespace c03_tuple_assign {
int hole() {
  std::tuple<V, int> t{std::string("hello"), 1};
  std::string &s = std::get<std::string>(std::get<0>(t));
  t = std::tuple<V, int>{V(K), 2};
  s[0] = 'Z'; // expected-error {{'s' points into 't', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(std::get<0>(t)));
}
} // namespace c03_tuple_assign

// lifetimes/c09_pair_vec
namespace c09_pair_vec {
using P = std::pair<V, int>;
int hole() {
  std::vector<P> ps;
  ps.emplace_back(std::string("hello"), 1);
  std::string &s = std::get<std::string>(ps[0].first);
  ps[0] = P(V(K), 2);
  s[0] = 'Z'; // expected-error {{'s' points into 'ps', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(ps[0].first));
}
} // namespace c09_pair_vec

// lifetimes/c10_pair_heap_uaf
namespace c10_pair_heap_uaf {
using P = std::pair<std::string, int>;
int hole() {
  P p{std::string("hello-a-long-string-not-in-sso-buffer-xxxxxxxx"), 1};
  std::string_view sv = p.first;
  p = P(std::string("other-long-string-not-in-sso-buffer-yyyyyyyyyy"), 2);
  std::string q(46, 'Q');
  std::printf("%.*s\n", (int)sv.size(), sv.data()); // expected-error {{'sv' points into 'p', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
} // namespace c10_pair_heap_uaf

// lifetimes/c11_pair_lvalue_assign
namespace c11_pair_lvalue_assign {
using P = std::pair<V, int>;
int hole() {
  P p{std::string("hello"), 1};
  P q{V(K), 2};
  std::string &s = std::get<std::string>(p.first);
  p = q;
  s[0] = 'Z'; // expected-error {{'s' points into 'p', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(p.first));
}
} // namespace c11_pair_lvalue_assign

// lifetimes/d05_any_assign_valcast
namespace d05_any_assign_valcast {
int hole() {
  V v = std::string("hello");
  std::any a;
  a = &v;
  std::string &s = std::get<std::string>(v);
  *std::any_cast<V *>(a) = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace d05_any_assign_valcast

// lifetimes/d06_any_local_ptr
namespace d06_any_local_ptr {
int hole() {
  V v = std::string("hello");
  std::any a = &v;
  V *p = std::any_cast<V *>(a);
  std::string &s = std::get<std::string>(v);
  *p = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace d06_any_local_ptr

// lifetimes/d10_tuple_vec_view
namespace d10_tuple_vec_view {
int hole() {
  std::tuple<std::vector<long>, int> t{std::vector<long>{1, 2, 3, 4, 5, 6, 7, 8}, 1};
  std::span<long> sp = std::get<0>(t);
  t = std::tuple<std::vector<long>, int>{std::vector<long>{9}, 2};
  std::vector<long> other{0x4444444444444444L, 0x4444444444444444L, 0x4444444444444444L, 0x4444444444444444L, 0x4444444444444444L, 0x4444444444444444L, 0x4444444444444444L, 0x4444444444444444L};
  std::printf("read %lx\n", sp[5]); // expected-error {{'sp' points into 't', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
} // namespace d10_tuple_vec_view

// lifetimes/e01_swap_pair
namespace e01_swap_pair {
using P = std::pair<V, int>;
int hole() {
  P p{std::string("hello"), 1};
  P q{V(K), 2};
  std::string &s = std::get<std::string>(p.first);
  std::swap(p, q);
  s[0] = 'Z'; // expected-error {{'s' was taken from 'p' before the call to 'swap', which may change what 'p' owns [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(p.first));
}
} // namespace e01_swap_pair

// lifetimes/e02_swap_tuple
namespace e02_swap_tuple {
using T = std::tuple<V, int>;
int hole() {
  T p{std::string("hello"), 1};
  T q{V(K), 2};
  std::string &s = std::get<std::string>(std::get<0>(p));
  std::swap(p, q);
  s[0] = 'Z'; // expected-error {{'s' was taken from 'p' before the call to 'swap', which may change what 'p' owns [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(std::get<0>(p)));
}
} // namespace e02_swap_tuple

// lifetimes/e03_vec_pair_push
namespace e03_vec_pair_push {
using P = std::pair<std::vector<long>, int>;
int hole() {
  P p{std::vector<long>{1, 2, 3, 4, 5, 6, 7, 8}, 1};
  long &r = p.first[5];
  p = P{std::vector<long>{9}, 2};
  std::vector<long> other(8, 0x4444444444444444L);
  std::printf("read %lx\n", r); // expected-error {{'r' points into 'p', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
} // namespace e03_vec_pair_push

// lifetimes/e07_base_ptr_member
namespace e07_base_ptr_member {
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &v) : v(v) {} void run() override { v = K; } };
struct Holder { Base &b; void go() { b.run(); } };
int hole() {
  V v = std::string("hello");
  D d(v);
  Holder h{d};
  std::string &s = std::get<std::string>(v);
  h.go();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace e07_base_ptr_member

// lifetimes/f05_holder_fn
namespace f05_holder_fn {
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &v) : v(v) {} void run() override { v = K; } };
struct Holder { Base &b; };
void go(Holder &h) { h.b.run(); }
int hole() {
  V v = std::string("hello");
  D d(v);
  Holder h{d};
  std::string &s = std::get<std::string>(v);
  go(h);
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through 'h' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace f05_holder_fn

// lifetimes/f11_holder_view_nonpoly
namespace f11_holder_view_nonpoly {
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &v) : v(v) {} void run() override { v = K; } };
struct M { V &v; void flip() { v = K; } };
struct Holder { M &m; void go() { m.flip(); } };
int hole() {
  V v = std::string("hello");
  M m{v};
  Holder h{m};
  std::string &s = std::get<std::string>(v);
  h.go();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace f11_holder_view_nonpoly

// lifetimes/f12_holder_ptr_base
namespace f12_holder_ptr_base {
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &v) : v(v) {} void run() override { v = K; } };
struct Holder { Base *b; void go() { b->run(); } };
int hole() {
  V v = std::string("hello");
  D d(v);
  Holder h{&d};
  std::string &s = std::get<std::string>(v);
  h.go();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace f12_holder_ptr_base

// lifetimes/g01_view2_fn
namespace g01_view2_fn {
struct M { V &v; void flip() { v = K; } };
struct Holder { M &m; };
void go(Holder &h) { h.m.flip(); }
int hole() {
  V v = std::string("hello");
  M m{v};
  Holder h{m};
  std::string &s = std::get<std::string>(v);
  go(h);
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through 'h' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace g01_view2_fn

// lifetimes/g02_view2_ptr
namespace g02_view2_ptr {
struct M { V &v; void flip() { v = K; } };
struct Holder { M *m; void go() { m->flip(); } };
int hole() {
  V v = std::string("hello");
  M m{v};
  Holder h{&m};
  std::string &s = std::get<std::string>(v);
  h.go();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace g02_view2_ptr

// lifetimes/g09_view3
namespace g09_view3 {
struct M { V &v; void flip() { v = K; } };
struct H1 { M &m; void go() { m.flip(); } };
struct H2 { H1 &h; void go() { h.go(); } };
int hole() {
  V v = std::string("hello");
  M m{v};
  H1 h1{m};
  H2 h2{h1};
  std::string &s = std::get<std::string>(v);
  h2.go();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'go' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace g09_view3

// lifetimes/g15_transform_view
namespace g15_transform_view {
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  std::vector<int> xs{1};
  auto r = xs | std::views::transform([&v](int x) { v = K; return x; });
  std::string &s = std::get<std::string>(v);
  for (int x : r) (void)x;
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'operator*', which may call '__begin1', which may change what '__begin1' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace g15_transform_view

// lifetimes/g19_mem_fn_ptr_view2
namespace g19_mem_fn_ptr_view2 {
struct M { V &v; void flip() { v = K; } };
struct Holder { M &m; void go() { m.flip(); } };
int hole() {
  V v = std::string("hello");
  M m{v};
  Holder h{m};
  auto pm = &Holder::go;
  std::string &s = std::get<std::string>(v);
  (h.*pm)();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call through a pointer to a member function may reach through 'h' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace g19_mem_fn_ptr_view2

// lifetimes/h01_filter_view
namespace h01_filter_view {
int hole() {
  V v = std::string("hello");
  std::vector<int> xs{1};
  auto r = xs | std::views::filter([&v](int) { v = K; return true; });
  std::string &s = std::get<std::string>(v);
  for (int x : r) (void)x;
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'end', which may call '__range1', which may change what '__range1' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace h01_filter_view

// lifetimes/h02_transform_begin
namespace h02_transform_begin {
int hole() {
  V v = std::string("hello");
  std::vector<int> xs{1};
  auto r = xs | std::views::transform([&v](int x) { v = K; return x; });
  std::string &s = std::get<std::string>(v);
  int y = *r.begin();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'operator*', which may call 'r.begin()', which may change what 'r.begin()' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx %d\n", std::get<long>(v), y);
}
} // namespace h02_transform_begin

// lifetimes/h03_transform_to
// (Where the library has std::ranges::to.)
#if defined(__cpp_lib_ranges_to_container)
namespace h03_transform_to {
int hole() {
  V v = std::string("hello");
  std::vector<int> xs{1};
  auto r = xs | std::views::transform([&v](int x) { v = K; return x; });
  std::string &s = std::get<std::string>(v);
  auto out = std::ranges::to<std::vector<int>>(r);
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'to', which may call 'r', which may change what 'r' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx %zu\n", std::get<long>(v), out.size());
}
} // namespace h03_transform_to
#endif

// lifetimes/h08_view2_transform
namespace h08_view2_transform {
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  M m{v};
  std::vector<int> xs{1};
  auto r = xs | std::views::transform([&m](int x) { m.flip(); return x; });
  std::string &s = std::get<std::string>(v);
  for (int x : r) (void)x;
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'operator*', which may call '__begin1', which may change what '__begin1' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace h08_view2_transform

// lifetimes/h10_transform_view_ctor
namespace h10_transform_view_ctor {
int hole() {
  V v = std::string("hello");
  std::vector<int> xs{1};
  auto lam = [&v](int x) { v = K; return x; };
  std::ranges::transform_view r(xs, lam);
  std::string &s = std::get<std::string>(v);
  for (int x : r) (void)x;
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'operator*', which may call '__begin1', which may change what '__begin1' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace h10_transform_view_ctor

// lifetimes/i03_tl_foreach_fnptr
namespace i03_tl_foreach_fnptr {
thread_local V g = std::string("hello");
void flip() { g = K; }
void flipInt(int) { g = K; }
int hole() {
  std::vector<int> xs{1};
  std::string &s = std::get<std::string>(g);
  std::for_each(xs.begin(), xs.end(), flipInt);
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to 'for_each' may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace i03_tl_foreach_fnptr

// lifetimes/i04_tl_fnptr_local
namespace i04_tl_fnptr_local {
thread_local V g = std::string("hello");
void flip() { g = K; }
void flipInt(int) { g = K; }
int hole() {
  void (*fp)() = flip;
  std::string &s = std::get<std::string>(g);
  fp();
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to a call may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace i04_tl_fnptr_local

// lifetimes/i05_tl_invoke
namespace i05_tl_invoke {
thread_local V g = std::string("hello");
void flip() { g = K; }
void flipInt(int) { g = K; }
int hole() {
  std::string &s = std::get<std::string>(g);
  std::invoke(flip);
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to 'invoke' may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace i05_tl_invoke

// lifetimes/i09_tl_lib_sort
namespace i09_tl_lib_sort {
thread_local V g = std::string("hello");
void flip() { g = K; }
void flipInt(int) { g = K; }
bool lessFlip(int a, int b) { g = K; return a < b; }
int hole() {
  std::vector<int> xs{2, 1};
  std::string &s = std::get<std::string>(g);
  std::sort(xs.begin(), xs.end(), lessFlip);
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to 'sort' may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace i09_tl_lib_sort

// lifetimes/i10_tl_dtor
namespace i10_tl_dtor {
thread_local V g = std::string("hello");
void flip() { g = K; }
void flipInt(int) { g = K; }
struct Flipper { ~Flipper() { g = K; } };
int hole() {
  std::string &s = std::get<std::string>(g);
  { Flipper f; }
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to 'Flipper' may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace i10_tl_dtor

// lifetimes/i11_tl_ctor
namespace i11_tl_ctor {
thread_local V g = std::string("hello");
void flip() { g = K; }
void flipInt(int) { g = K; }
struct Flipper { Flipper() { g = K; } };
int hole() {
  std::string &s = std::get<std::string>(g);
  Flipper f;
  s[0] = 'Z'; // expected-error {{'s' points into the global 'g', which the call to 'Flipper' may change [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(g));
}
} // namespace i11_tl_ctor

// lifetimes/j01_view_dtor
namespace j01_view_dtor {
struct F { V &v; ~F() { v = K; } };
int hole() {
  V v = std::string("hello");
  std::string &s = std::get<std::string>(v);
  { F f{v}; }
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to '~F' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace j01_view_dtor

// lifetimes/j04_view_dtor_uptr
namespace j04_view_dtor_uptr {
struct F { V &v; explicit F(V &v) : v(v) {} ~F() { v = K; } };
int hole() {
  V v = std::string("hello");
  auto u = std::make_unique<F>(v);
  std::string &s = std::get<std::string>(v);
  u.reset();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'reset' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace j04_view_dtor_uptr

// lifetimes/j05_view_dtor_temp
namespace j05_view_dtor_temp {
struct F { V &v; ~F() { v = K; } };
int hole() {
  V v = std::string("hello");
  std::string &s = std::get<std::string>(v);
  (void)F{v};
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to '~F' may reach through its object [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace j05_view_dtor_temp
