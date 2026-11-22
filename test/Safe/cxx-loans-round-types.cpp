// RFC 0053's own G4 round (`build/rfc53/g4/types`), the holes its amendments 13
// to 19 reject: a functor over a `const std::function`, `mutable` inside the
// library's classes and in bases, a `const` global with a `mutable` member,
// constructors, destructors and pointers to functions that change a global, a
// `std::any` read by value, spans of arrays of views.
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

// types/p04_functor_const_function_ptr
namespace p04_functor_const_function_ptr {
struct Call { const std::function<void()> *f; void operator()() const { (*f)(); } };
int hole() {
  V v = std::string(48, 'x');
  std::function<void()> fn = [&v] { v = K; };
  Call c{&fn};
  const std::string &s = std::get<std::string>(v);
  c();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'c', which may change what 'c' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace p04_functor_const_function_ptr

// types/p06_any_refwrap_cast_copy
namespace p06_any_refwrap_cast_copy {
int hole() {
  V v = std::string(48, 'x');
  std::any a = std::ref(v);
  const std::string &s = std::get<std::string>(v);
  std::any_cast<std::reference_wrapper<V>>(a).get() = K;
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
} // namespace p06_any_refwrap_cast_copy

// types/p09_mutable_in_optional
namespace p09_mutable_in_optional {
struct In { mutable V v; };
static void f(const std::optional<In> &o) { o->v = K; }
int hole() {
  std::optional<In> o = In{std::string(48, 'x')};
  const std::string &s = std::get<std::string>(o->v);
  f(o);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'o' before the call to 'f', which may change what 'o' owns [weavec-safe:stale-borrow]}}
}
} // namespace p09_mutable_in_optional

// types/p10_mutable_in_base
namespace p10_mutable_in_base {
struct B { mutable V v; };
struct D : B {};
static void f(const D &d) { d.v = K; }
int hole() {
  D d;
  d.v = std::string(48, 'x');
  const std::string &s = std::get<std::string>(d.v);
  f(d);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'd' before the call to 'f', which may change what 'd' owns [weavec-safe:stale-borrow]}}
}
} // namespace p10_mutable_in_base

// types/p11_mutable_in_vector
namespace p11_mutable_in_vector {
struct In { mutable V v; };
static void f(const std::vector<In> &o) { o[0].v = K; }
int hole() {
  std::vector<In> o;
  o.push_back(In{std::string(48, 'x')});
  const std::string &s = std::get<std::string>(o[0].v);
  f(o);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'o' before the call to 'f', which may change what 'o' owns [weavec-safe:stale-borrow]}}
}
} // namespace p11_mutable_in_vector

// types/p12_mutable_in_pair
namespace p12_mutable_in_pair {
struct In { mutable V v; };
static void f(const std::pair<In, int> &o) { o.first.v = K; }
int hole() {
  std::pair<In, int> o{In{std::string(48, 'x')}, 1};
  const std::string &s = std::get<std::string>(o.first.v);
  f(o);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'o' before the call to 'f', which may change what 'o' owns [weavec-safe:stale-borrow]}}
}
} // namespace p12_mutable_in_pair

// types/p13_lambda_asconst_mutable
namespace p13_lambda_asconst_mutable {
struct Box { mutable V v; };
static void touch(const Box &b) { b.v = K; }
int hole() {
  Box box{std::string(48, 'x')};
  auto l = [&b = std::as_const(box)] { touch(b); };
  const std::string &s = std::get<std::string>(box.v);
  l();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'box' before the call of 'l', which changes 'box' [weavec-safe:stale-borrow]}}
}
} // namespace p13_lambda_asconst_mutable

// types/p14_functor_const_optional_function
namespace p14_functor_const_optional_function {
struct Call {
  const std::optional<std::function<void()>> *f;
  void operator()() const { (**f)(); }
};
int hole() {
  V v = std::string(48, 'x');
  std::function<void()> fn = [&v] { v = K; };
  std::optional<std::function<void()>> o = fn;
  Call c{&o};
  const std::string &s = std::get<std::string>(v);
  c();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'c', which may change what 'c' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace p14_functor_const_optional_function

// types/p25_ctor_changes_global
namespace p25_ctor_changes_global {
thread_local V g = std::string(48, 'x');
struct T { T() { g = K; } };
int hole() {
  const std::string &s = std::get<std::string>(g);
  T t;
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to 'T' may change [weavec-safe:stale-borrow]}}
}
} // namespace p25_ctor_changes_global

// types/p26_dtor_changes_global
namespace p26_dtor_changes_global {
thread_local V g = std::string(48, 'x');
struct T { int x = 0; ~T() { g = K; } };
int hole() {
  const std::string &s = std::get<std::string>(g);
  { T t; }
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to 'T' may change [weavec-safe:stale-borrow]}}
}
} // namespace p26_dtor_changes_global

// types/p28_const_global_mutable
namespace p28_const_global_mutable {
struct Box { mutable V v; };
thread_local const Box gb{std::string(48, 'x')};
static void flip() { gb.v = K; } // expected-error {{'gb.v' is a global, which every function can reach, and 'operator=' may change what it owns [weavec-safe:owner-path]}}
int hole() {
  const std::string &s = std::get<std::string>(gb.v);
  flip();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'gb', which the call to 'flip' may change [weavec-safe:stale-borrow]}}
}
} // namespace p28_const_global_mutable

// types/p29_fnptr_global
namespace p29_fnptr_global {
thread_local V g = std::string(48, 'x');
static void flip() { g = K; }
int hole() {
  void (*fp)() = flip;
  const std::string &s = std::get<std::string>(g);
  fp();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to a call may change [weavec-safe:stale-borrow]}}
}
} // namespace p29_fnptr_global

// types/p30_invoke_global
namespace p30_invoke_global {
thread_local V g = std::string(48, 'x');
static void flip() { g = K; }
int hole() {
  const std::string &s = std::get<std::string>(g);
  std::invoke(flip);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to 'invoke' may change [weavec-safe:stale-borrow]}}
}
} // namespace p30_invoke_global

// types/p38_any_span
namespace p38_any_span {
int hole() {
  std::array<V, 1> arr{std::string(48, 'x')};
  std::any a = std::span<V>(arr);
  const std::string &s = std::get<std::string>(arr[0]);
  std::any_cast<std::span<V>>(a)[0] = K;
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into 'arr', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
} // namespace p38_any_span

// types/p43_bind_cref_mutable_base
namespace p43_bind_cref_mutable_base {
struct B { mutable V v; };
struct D : B {};
static void touch(const D &d) { d.v = K; }
int hole() {
  D d;
  d.v = std::string(48, 'x');
  auto b = std::bind(touch, std::cref(d));
  const std::string &s = std::get<std::string>(d.v);
  b();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'd' before the call of 'b', which may change what 'b' holds, 'd' [weavec-safe:stale-borrow]}}
}
} // namespace p43_bind_cref_mutable_base

// types/p44_global_view_struct_result
namespace p44_global_view_struct_result {
struct R { V &r; };
static R get() { thread_local V g = std::string(48, 'x'); return R{g}; }
int hole() {
  R a = get();
  const std::string &s = std::get<std::string>(a.r);
  R b = get();
  b.r = K;
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into a global, which the call to 'get' may change [weavec-safe:stale-borrow]}}
}
} // namespace p44_global_view_struct_result

// types/p46_optional_reset_dtor_global
namespace p46_optional_reset_dtor_global {
thread_local V g = std::string(48, 'x');
struct T { int x = 0; ~T() { g = K; } };
int hole() {
  std::optional<T> o;
  o.emplace();
  const std::string &s = std::get<std::string>(g);
  o.reset();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to 'reset' may change [weavec-safe:stale-borrow]}}
}
} // namespace p46_optional_reset_dtor_global

// types/p47_vector_copy_ctor_global
namespace p47_vector_copy_ctor_global {
thread_local V g = std::string(48, 'x');
struct T {
  int x = 0;
  T() = default;
  T(const T &o) : x(o.x) { g = K; }
};
int hole() {
  std::vector<T> ts;
  T t;
  const std::string &s = std::get<std::string>(g);
  ts.push_back(t);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to 'push_back' may change [weavec-safe:stale-borrow]}}
}
} // namespace p47_vector_copy_ctor_global

// types/p48_static_member_global
namespace p48_static_member_global {
struct S { static thread_local V g; static void flip() { g = K; } };
thread_local V S::g = std::string(48, 'x');
int hole() {
  void (*fp)() = &S::flip;
  const std::string &s = std::get<std::string>(S::g);
  fp();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into the global 'g', which the call to a call may change [weavec-safe:stale-borrow]}}
}
} // namespace p48_static_member_global

// types/p61_any_refwrap_copy_local
namespace p61_any_refwrap_copy_local {
int hole() {
  V v = std::string(48, 'x');
  std::any a = std::ref(v);
  const std::string &s = std::get<std::string>(v);
  auto r = std::any_cast<std::reference_wrapper<V>>(a);
  r.get() = K;
  std::cout << "s = " << s << "\n"; // expected-error {{'s' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
} // namespace p61_any_refwrap_copy_local

// types/p63_span_of_lambdas
namespace p63_span_of_lambdas {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::array<decltype(l), 1> ls{l};
  std::span<decltype(l)> sp(ls);
  const std::string &s = std::get<std::string>(v);
  sp[0]();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call to 'operator[]', which may call 'sp', which may change what 'sp' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace p63_span_of_lambdas
