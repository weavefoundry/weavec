// RFC 0053 §5 (D4): a container, optional, pair, array or smart pointer of the
// library whose elements are views or pointers is a view of what it is made
// from (from braces, by conversion, as a result, through an out parameter);
// what it gives access to is where its loans point; it is not returned or
// kept past what it points to.
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

// lifetimes/q44_vec_refwrap
namespace q44_vec_refwrap {
int hole() {
  V v = std::string("hello");
  std::vector<std::reference_wrapper<V>> rs{v};
  std::string &s = std::get<std::string>(v);
  rs[0].get() = K; // expected-error {{'rs' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q44_vec_refwrap

// lifetimes/r14_refwrap_vec
namespace r14_refwrap_vec {
void dummy() {}
int hole() {
  V v = std::string("hello");
  std::vector<std::reference_wrapper<V>> rs{v}; std::string &s = std::get<std::string>(rs[0].get()); // expected-error {{'rs' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
  v = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r14_refwrap_vec

// lifetimes/w05_refwrap_loop
namespace w05_refwrap_loop {
int hole() {
  V v = std::string("hello");
  std::vector<std::reference_wrapper<V>> rs{v};
  std::string &s = std::get<std::string>(v);
  for (auto r : rs) r.get() = K; // expected-error {{'rs' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}} expected-error {{'__range1' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}} expected-error {{'__begin1' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}} expected-error {{'__end1' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace w05_refwrap_loop

// lifetimes/w14_refwrap_vec_begin
namespace w14_refwrap_vec_begin {
int hole() {
  V v = std::string("hello");
  std::vector<std::reference_wrapper<V>> rs{v};
  std::string &s = std::get<std::string>(v);
  rs.begin()->get() = K; // expected-error {{'rs' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace w14_refwrap_vec_begin

// lifetimes/w16_array_refwrap
namespace w16_array_refwrap {
int hole() {
  V v = std::string("hello");
  std::array<std::reference_wrapper<V>, 1> rs{std::ref(v)};
  std::string &s = std::get<std::string>(v);
  rs[0].get() = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace w16_array_refwrap

// lifetimes/w17_opt_refwrap
namespace w17_opt_refwrap {
int hole() {
  V v = std::string("hello");
  std::optional<std::reference_wrapper<V>> o = std::ref(v);
  std::string &s = std::get<std::string>(v);
  o->get() = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace w17_opt_refwrap

// lifetimes/w18_vec_span
namespace w18_vec_span {
int hole() {
  V v = std::string("hello");
  std::vector<std::span<V>> ss{std::span<V>(&v, 1)};
  std::string &s = std::get<std::string>(v);
  ss[0][0] = K; // expected-error {{'ss' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace w18_vec_span

// lifetimes/r09_outrefwrap
namespace r09_outrefwrap {
void g(V &v, std::optional<std::reference_wrapper<std::string>> &o) { o = std::ref(std::get<std::string>(v)); } // expected-error {{'o' would keep a borrow of the parameter 'v', which is lent for the call only [weavec-safe:escape]}}
int hole() {
  V v = std::string("hello");
  std::optional<std::reference_wrapper<std::string>> o; g(v, o); std::string &s = o->get();
  v = K;
  s[0] = 'Z';
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r09_outrefwrap

// lifetimes/r17_pair_ptr_ret
namespace r17_pair_ptr_ret {
std::pair<std::string *, int> g(V &v) { return {&std::get<std::string>(v), 1}; }
int hole() {
  V v = std::string("hello");
  auto pr = g(v); std::string &s = *pr.first;
  v = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r17_pair_ptr_ret

// lifetimes/r18_vec_ptr_ret
namespace r18_vec_ptr_ret {
std::vector<std::string *> g(V &v) { return {&std::get<std::string>(v)}; }
int hole() {
  V v = std::string("hello");
  auto vs = g(v); std::string &s = *vs[0];
  v = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r18_vec_ptr_ret

// lifetimes/r19_optional_ptr_ret
namespace r19_optional_ptr_ret {
std::optional<std::string *> g(V &v) { return &std::get<std::string>(v); }
int hole() {
  V v = std::string("hello");
  auto o = g(v); std::string &s = **o;
  v = K;
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace r19_optional_ptr_ret

// lifetimes/q20_ret_vec_ptr
namespace q20_ret_vec_ptr {
std::vector<long *> mk() { long x = 5; return {&x}; } // expected-error {{'{&x}' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
long smash() { volatile long a[8]; for (int i = 0; i < 8; i++) a[i] = 0x2222; return a[3]; }
int hole() {
  auto v = mk();
  smash();
  std::printf("%lx\n", *v[0]);
}
} // namespace q20_ret_vec_ptr

// lifetimes/v25_ret_vec_ptr_frame
namespace v25_ret_vec_ptr_frame {
__attribute__((noinline)) std::vector<long *> mk() { long x = 5; return {&x}; } // expected-error {{'{&x}' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
template <class R> __attribute__((noinline)) void victim(R &r) { volatile long a[8] = {1, 1, 1, 1, 1, 1, 1, 1}; *r[0] = 0x4444444444444444L; long m = 0; for (int i = 0; i < 8; i++) m |= a[i]; std::printf("victim locals OR (should be 1): %lx\n", m); }
int hole() { auto r = mk(); victim(r); }
} // namespace v25_ret_vec_ptr_frame

// lifetimes/v01_ret_pair_ptr
namespace v01_ret_pair_ptr {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
__attribute__((noinline)) std::pair<long *, int> mk() { long x = 5; return {&x, 1}; } // expected-error {{'{&x, 1}' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
int hole() {
  auto r = mk(); smash(); std::printf("%lx\n", *r.first);
}
} // namespace v01_ret_pair_ptr

// lifetimes/v02_ret_opt_ptr
namespace v02_ret_opt_ptr {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
__attribute__((noinline)) std::optional<long *> mk() { long x = 5; return &x; } // expected-error {{'&x' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
int hole() {
  auto r = mk(); smash(); std::printf("%lx\n", **r);
}
} // namespace v02_ret_opt_ptr

// lifetimes/v03_ret_array_ptr
namespace v03_ret_array_ptr {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
__attribute__((noinline)) std::array<long *, 1> mk() { long x = 5; return {&x}; } // expected-error {{'{&x}' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
int hole() {
  auto r = mk(); smash(); std::printf("%lx\n", *r[0]);
}
} // namespace v03_ret_array_ptr

// lifetimes/v18_ret_unique_ptr_ptr
namespace v18_ret_unique_ptr_ptr {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
__attribute__((noinline)) std::unique_ptr<long *> mk() { long x = 5; return std::make_unique<long *>(&x); } // expected-error {{'std::make_unique<long *>(&x)' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
int hole() {
  auto r = mk(); smash(); std::printf("%lx\n", **r);
}
} // namespace v18_ret_unique_ptr_ptr

// lifetimes/v08_ret_vec_sv
namespace v08_ret_vec_sv {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
__attribute__((noinline)) std::vector<std::string_view> mk() { std::string s(40, 'A'); return {s}; } // expected-error {{what is returned points into 's', which does not outlive the function [weavec-safe:escape]}}
int hole() {
  auto r = mk(); std::string o(40, 'Q'); std::printf("%.*s\n", (int)r[0].size(), r[0].data());
}
} // namespace v08_ret_vec_sv

// lifetimes/v13_store_ptrstruct
namespace v13_store_ptrstruct {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
struct Keep { long *p = nullptr; };
int hole() {
  Keep k; { long x = 5; k.p = &x; } smash(); std::printf("%lx\n", *k.p); // expected-error {{'k' points into 'x', whose lifetime has ended [weavec-safe:stale-borrow]}}
}
} // namespace v13_store_ptrstruct

// lifetimes/v13b_store_ptrstruct_reuse
namespace v13b_store_ptrstruct_reuse {
struct Keep { long *p = nullptr; };
int hole() {
  Keep k;
  { long x = 5; k.p = &x; }
  auto other = std::make_unique<long>(0x3333333333333333L);
  *k.p = 0x4444444444444444L;           // through the stale pointer into a dead local // expected-error {{'k' points into 'x', whose lifetime has ended [weavec-safe:stale-borrow]}}
  std::printf("other (never written) %lx\n", *other);
}
} // namespace v13b_store_ptrstruct_reuse

// lifetimes/v14_store_alias_sp
namespace v14_store_alias_sp {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }

int hole() {
  std::shared_ptr<long> sp; { long x = 5; sp = std::shared_ptr<long>(std::shared_ptr<long>(), &x); } smash(); std::printf("%lx\n", *sp); // expected-error {{'sp' points into 'x', whose lifetime has ended [weavec-safe:stale-borrow]}}
}
} // namespace v14_store_alias_sp

// lifetimes/v14b_store_alias_sp_scope
namespace v14b_store_alias_sp_scope {
int hole() {
  std::shared_ptr<long> sp;
  { long x = 5; sp = std::shared_ptr<long>(std::shared_ptr<long>(), &x); }
  { long y = 0x3333333333333333L; *sp = 0x4444444444444444L; std::printf("y (never written) %lx\n", y); } // expected-error {{'sp' points into 'x', whose lifetime has ended [weavec-safe:stale-borrow]}}
}
} // namespace v14b_store_alias_sp_scope

// lifetimes/v15_store_pair
namespace v15_store_pair {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
void dummy() {}
int hole() {
  std::pair<long *, int> k{nullptr, 0}; { long x = 5; k = {&x, 1}; } smash(); std::printf("%lx\n", *k.first); // expected-error {{'k' points into 'x', whose lifetime has ended [weavec-safe:stale-borrow]}}
}
} // namespace v15_store_pair

// lifetimes/v16_store_vec_ptr
namespace v16_store_vec_ptr {
__attribute__((noinline)) long smash() { volatile long a[32]; for (int i = 0; i < 32; i++) a[i] = 0x2222222222222222L; return a[3]; }
void dummy() {}
int hole() {
  std::vector<long *> k; { long x = 5; k.push_back(&x); } smash(); std::printf("%lx\n", *k[0]); // expected-error {{'k' points into 'x', whose lifetime has ended [weavec-safe:stale-borrow]}}
}
} // namespace v16_store_vec_ptr

// lifetimes/v21_ret_vec_refwrap
namespace v21_ret_vec_refwrap {
__attribute__((noinline)) std::vector<std::reference_wrapper<long>> mk() { long x = 5; return {std::ref(x)}; } // expected-error {{what is returned points into 'x', which does not outlive the function [weavec-safe:escape]}}
template <class R> __attribute__((noinline)) void victim(R &r) { volatile long a[8] = {1, 1, 1, 1, 1, 1, 1, 1}; r[0].get() = 0x4444444444444444L; long m = 0; for (int i = 0; i < 8; i++) m |= a[i]; std::printf("victim locals OR (should be 1): %lx\n", m); }
int hole() { auto r = mk(); victim(r); }
} // namespace v21_ret_vec_refwrap

// lifetimes/v22_ret_vec_span
namespace v22_ret_vec_span {
__attribute__((noinline)) std::vector<std::span<long>> mk() { long x[2] = {5, 6}; return {std::span<long>(x)}; } // expected-error {{what is returned points into 'x', which does not outlive the function [weavec-safe:escape]}}
template <class R> __attribute__((noinline)) void victim(R &r) { volatile long a[8] = {1, 1, 1, 1, 1, 1, 1, 1}; r[0][0] = 0x4444444444444444L; long m = 0; for (int i = 0; i < 8; i++) m |= a[i]; std::printf("victim locals OR (should be 1): %lx\n", m); }
int hole() { auto r = mk(); victim(r); }
} // namespace v22_ret_vec_span
