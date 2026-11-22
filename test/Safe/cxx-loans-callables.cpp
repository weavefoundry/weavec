// RFC 0053 §1 to §4 (D1, D2, D3): a callable of any type (a lambda in a pair,
// an optional, an array or behind a pointer, a binder, a functor, a pointer to a
// member function, a factory's result) is a view, and its call, or a call it
// is passed to, ends the borrows into what its loans reach; a capture the body
// provably only reads ends nothing; a callable passed beside what its loans
// reach is `call-alias`; a library function given a callable calls it.
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

// lifetimes/q06_lambda_refwrap
namespace q06_lambda_refwrap {
int hole() {
  V v = std::string("hello");
  auto f = [r = std::ref(v)] { r.get() = K; };
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which changes 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q06_lambda_refwrap

// lifetimes/q07_lambda_factory
namespace q07_lambda_factory {
int hole() {
  V v = std::string("hello");
  auto mk = [&v] { return [&v] { v = K; }; };
  auto f = mk();
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q07_lambda_factory

// lifetimes/q08_fn_factory
namespace q08_fn_factory {
auto mk(V &v) { return [&v] { v = K; }; }
int hole() {
  V v = std::string("hello");
  auto f = mk(v);
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q08_fn_factory

// lifetimes/q09_bind
namespace q09_bind {
void set(V &v) { v = K; }
int hole() {
  V v = std::string("hello");
  auto f = std::bind(set, std::ref(v));
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q09_bind

// lifetimes/q10_bind_front
namespace q10_bind_front {
void set(V &v) { v = K; }
int hole() {
  V v = std::string("hello");
  auto f = std::bind_front(set, std::ref(v));
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q10_bind_front

// lifetimes/q21_fn_from_factory
namespace q21_fn_from_factory {
auto mk(V &v) { return [&v] { v = K; }; }
int hole() {
  V v = std::string("hello");
  std::function<void()> f = mk(v);
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q21_fn_from_factory

// lifetimes/q26_lambda_ptr
namespace q26_lambda_ptr {
int hole() {
  V v = std::string("hello");
  auto g = [&v] { v = K; }; auto *fp = &g;
  std::string &s = std::get<std::string>(v);
  (*fp)();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of '*fp', which may change what '*fp' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q26_lambda_ptr

// lifetimes/q28_memfnptr
namespace q28_memfnptr {
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  M m{v}; auto pm = &M::flip;
  std::string &s = std::get<std::string>(v);
  (m.*pm)();
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call through a pointer to a member function may reach through 'm' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q28_memfnptr

// lifetimes/q29_invoke_memfn
namespace q29_invoke_memfn {
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  M m{v};
  std::string &s = std::get<std::string>(v);
  std::invoke(&M::flip, m);
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'invoke' may reach through 'm' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q29_invoke_memfn

// lifetimes/q30_memfn
namespace q30_memfn {
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  M m{v};
  std::string &s = std::get<std::string>(v);
  std::mem_fn(&M::flip)(m);
  s[0] = 'Z'; // expected-error {{'s' points into 'v', which the call to 'operator()' may reach through 'm' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q30_memfn

// lifetimes/q31_cap_span
namespace q31_cap_span {
int hole() {
  V v = std::string("hello");
  auto f = [sp = std::span<V>(&v, 1)] { sp[0] = K; };
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which changes 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q31_cap_span

// lifetimes/q35b_run_factory
namespace q35b_run_factory {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }
auto mk(V &v) { return [&v] { v = K; }; }
int hole() {
  V v = std::string("hello");
  run(v, mk(v)); // expected-error {{'mk(v)' may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q35b_run_factory

// lifetimes/q36b_run_refwrap
namespace q36b_run_refwrap {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }

int hole() {
  V v = std::string("hello");
  run(v, [r = std::ref(v)] { r.get() = K; }); // expected-error {{the lambda may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q36b_run_refwrap

// lifetimes/q39b_run_bind
namespace q39b_run_bind {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }
void set(V &v) { v = K; }
int hole() {
  V v = std::string("hello");
  run(v, std::bind(set, std::ref(v))); // expected-error {{'std::bind(set, std::ref(v))' may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q39b_run_bind

// lifetimes/q54_run_span
namespace q54_run_span {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }

int hole() {
  V v = std::string("hello");
  run(v, [sp = std::span<V>(&v, 1)] { sp[0] = K; }); // expected-error {{the lambda may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q54_run_span

// lifetimes/q55_run_lambdaptr
namespace q55_run_lambdaptr {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }

int hole() {
  V v = std::string("hello");
  auto g = [&v] { v = K; }; auto *gp = &g; run(v, [gp] { (*gp)(); }); // expected-error {{the lambda may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q55_run_lambdaptr

// lifetimes/q56_run_memfn
namespace q56_run_memfn {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }
struct M { V &v; void flip() { v = K; } };
int hole() {
  V v = std::string("hello");
  M m{v}; run(v, std::bind_front(&M::flip, m)); // expected-error {{'std::bind_front(&M::flip, m)' may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q56_run_memfn

// lifetimes/q57_run_fnfactory
namespace q57_run_fnfactory {
void run(V &v, const std::function<void()> &cb) { std::string &s = std::get<std::string>(v); cb(); s[0] = 'Z'; }
auto mk(V &v) { return [&v] { v = K; }; }
int hole() {
  V v = std::string("hello");
  auto f = mk(v); run(v, f); // expected-error {{'f' may change 'v', which is passed with it [weavec-safe:call-alias]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace q57_run_fnfactory

// lifetimes/c12_cond_lambda
namespace c12_cond_lambda {
int hole() {
  V v = std::string("hello");
  auto a = [&v] { v = K; }; auto b = [] {}; std::function<void()> f = (v.index() == 0) ? std::function<void()>(a) : std::function<void()>(b);
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace c12_cond_lambda

// lifetimes/c14_pair_lambda
namespace c14_pair_lambda {
int hole() {
  V v = std::string("hello");
  auto g = [&v] { v = K; }; std::pair<decltype(g), int> pr{g, 1};
  std::string &s = std::get<std::string>(v);
  pr.first();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'pr.first', which may change what 'pr.first' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace c14_pair_lambda

// lifetimes/c17_lambda_in_vec_local
namespace c17_lambda_in_vec_local {
int hole() {
  V v = std::string("hello");
  auto g = [&v] { v = K; }; std::vector<decltype(g)> gs{g};
  std::string &s = std::get<std::string>(v);
  gs[0](); // expected-error {{'gs' points into a temporary 'const value_type[1]', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'gs[0]', which may change what 'gs[0]' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace c17_lambda_in_vec_local

// lifetimes/c19_lambda_ptr_param
namespace c19_lambda_ptr_param {
void call(auto *fp) { (*fp)(); }
int hole() {
  V v = std::string("hello");
  auto g = [&v] { v = K; };
  std::string &s = std::get<std::string>(v);
  call(&g);
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'call', which may call '&g', which may change what '&g' holds, 'v' [weavec-safe:stale-borrow]}}
  std::printf("long now %lx\n", std::get<long>(v));
}
} // namespace c19_lambda_ptr_param

// types/t02_lambda_in_pair
namespace t02_lambda_in_pair {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::pair<decltype(l), int> p{l, 0};
  const std::string &s = std::get<std::string>(v);
  p.first();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'p.first', which may change what 'p.first' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t02_lambda_in_pair

// types/t21_any_lambda
namespace t21_any_lambda {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::any a = l;
  const std::string &s = std::get<std::string>(v);
  std::any_cast<decltype(l) &>(a)();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'std::any_cast<decltype(l) &>(a)', which may change what 'std::any_cast<decltype(l) &>(a)' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t21_any_lambda

// types/t53_function_from_lambda_local
namespace t53_function_from_lambda_local {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::function<void()> f = std::ref(l);
  const std::string &s = std::get<std::string>(v);
  f();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t53_function_from_lambda_local

// types/t54_returned_lambda
namespace t54_returned_lambda {
static auto mk(V &v) { return [&v] { v = K; }; }
int hole() {
  V v = std::string(48, 'x');
  auto f = mk(v);
  const std::string &s = std::get<std::string>(v);
  f();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'f', which may change what 'f' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t54_returned_lambda

// types/t56_optional_lambda
namespace t56_optional_lambda {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::optional<decltype(l)> o = l;
  const std::string &s = std::get<std::string>(v);
  (*o)();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of '* o', which may change what '* o' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t56_optional_lambda

// types/t62_variant_of_lambda
namespace t62_variant_of_lambda {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::variant<decltype(l), long> w = l;
  const std::string &s = std::get<std::string>(v);
  std::get<0>(w)();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'std::get<0>(w)', which may change what 'std::get<0>(w)' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t62_variant_of_lambda

// types/t63_array_of_lambda
namespace t63_array_of_lambda {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  std::array<decltype(l), 1> a{l};
  const std::string &s = std::get<std::string>(v);
  a[0]();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'a[0]', which may change what 'a[0]' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t63_array_of_lambda

// types/t64_lambda_through_identity
namespace t64_lambda_through_identity {
template <class F> static F &id(F &f) { return f; }
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  auto &r = id(l);
  const std::string &s = std::get<std::string>(v);
  r();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'r', which may change what 'r' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t64_lambda_through_identity

// types/t65_lambda_ternary
namespace t65_lambda_ternary {
int main(int argc, char **) {
  V v = std::string(48, 'x');
  auto l = [&v] { v = K; };
  auto &r = argc > 0 ? l : l;
  const std::string &s = std::get<std::string>(v);
  r();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'r', which may change what 'r' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t65_lambda_ternary

// types/t66_lambda_local_alias
namespace t66_lambda_local_alias {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { V &r = v; r = K; };
  const std::string &s = std::get<std::string>(v);
  l();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'l', which changes 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t66_lambda_local_alias

// types/t67_lambda_local_ptr
namespace t67_lambda_local_ptr {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v] { V *p = &v; *p = K; };
  const std::string &s = std::get<std::string>(v);
  l();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'l', which changes 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t67_lambda_local_ptr

// types/t68_lambda_param_default
namespace t68_lambda_param_default {
int hole() {
  V v = std::string(48, 'x');
  auto l = [&v](long n) { auto &&r = v; r = n; };
  const std::string &s = std::get<std::string>(v);
  l(K);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'l', which changes 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t68_lambda_param_default

// types/t04_lambda_ref_wrapper_capture
namespace t04_lambda_ref_wrapper_capture {
int hole() {
  V v = std::string(48, 'x');
  auto l = [r = std::ref(v)] { r.get() = K; };
  const std::string &s = std::get<std::string>(v);
  l();
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call of 'l', which changes 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t04_lambda_ref_wrapper_capture

// types/t19_visit_functor_ref
namespace t19_visit_functor_ref {
struct Vis { V &v; void operator()(long) const {} void operator()(const std::string &) const { v = K; } };
int hole() {
  V v = std::string(48, 'x');
  V other = std::string("y");
  const std::string &s = std::get<std::string>(v);
  std::visit(Vis{v}, other);
  std::cout << "s = " << s << "\n"; // expected-error {{'s' was taken from 'v' before the call to 'visit', which may call 'Vis{v}', which may change what 'Vis{v}' holds, 'v' [weavec-safe:stale-borrow]}}
}
} // namespace t19_visit_functor_ref
