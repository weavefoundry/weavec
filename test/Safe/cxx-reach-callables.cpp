// RFC 0052 §5.1, §5.2: what a lambda the function sees may change ends the
// borrows into it where the lambda is called and where it is passed (every
// instance of a generic lambda's call operator is read); a callable passed
// beside what it may change is `call-alias` for every callee; a callee
// believes the callables it is lent. The round of 2026-11-21's
// `lifetimes/p09`, `p10`, `p11`, `p12`, `p15`, `p16`, `p75`, `p76`, `p78`
// and `types/p06`, `p07`, `p08`.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <array>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using V = std::variant<std::string, long>;
constexpr long K = 0x1111111111111111L;

// p09: a lambda that changes what it captured
void calledLambda() {
  V v = std::string("hello");
  auto flip = [&v] { v = K; };
  std::string &s = std::get<std::string>(v);
  flip();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'flip', which changes 'v' [weavec-safe:stale-borrow]}}
}
// p11: a lambda that captures a value it is given
void forged(unsigned long address) {
  std::variant<std::string, std::array<unsigned long, 3>> v = std::string("hi");
  auto forge = [&v, address] {
    v = std::array<unsigned long, 3>{address, 40, 0x8000000000000030UL};
  };
  std::string &s = std::get<std::string>(v);
  forge();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'forge', which changes 'v' [weavec-safe:stale-borrow]}}
}
// p12, types/p07: through a `std::function` the function initialized
void erasedLocal() {
  V v = std::string("hello");
  std::function<void()> flip = [&v] { v = K; };
  std::string &s = std::get<std::string>(v);
  flip();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'flip', which changes 'v' [weavec-safe:stale-borrow]}}
}
// p15: a capture by default, called where it is written
void defaultCapture() {
  V v = std::string("hello");
  std::string &s = std::get<std::string>(v);
  [&] { v = K; }();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of the lambda, which changes 'v' [weavec-safe:stale-borrow]}}
}
// p75: passed to a function of the program, which may call it
void run(const std::function<void()> &cb) { cb(); }
void passedToRun() {
  V v = std::string("hello");
  std::function<void()> flip = [&v] { v = K; };
  std::string &r = std::get<std::string>(v);
  run(flip);
  r[0] = 'Z'; // expected-error {{'r' was taken from 'v' before the call to 'run', which may call 'flip', which changes 'v' [weavec-safe:stale-borrow]}}
}
// p76: passed to the library, which may call it
void invoked() {
  V v = std::string("hello");
  std::string &r = std::get<std::string>(v);
  std::invoke([&v] { v = K; });
  r[0] = 'Z'; // expected-error {{'r' was taken from 'v' before the call to 'invoke', which may call the lambda, which changes 'v' [weavec-safe:stale-borrow]}}
}
// p78: a lambda that captures `this`
struct Holder {
  V v = std::string("hello");
  void flipThrough() {
    std::string &r = std::get<std::string>(v);
    auto f = [this] { v = K; };
    f();
    r[0] = 'Z'; // expected-error {{'r' was taken from 'v' before the call of 'f', which changes 'v' [weavec-safe:stale-borrow]}}
  }
  // Only the members the body changes.
  std::string other = "x";
  void flipOther() {
    std::string &r = std::get<std::string>(v);
    auto f = [this] { other = "y"; };
    f();
    r[0] = 'Z';
  }
};
// p10: a callable beside the object it changes, for every callee
void believes(V &v, const std::function<void()> &cb) {
  std::string &s = std::get<std::string>(v);
  cb(); // a lent callable reaches nothing the other parameters do (X-A2)
  s[0] = 'Z';
}
void passesBoth() {
  V v = std::string("hello");
  believes(v, [&v] { v = K; }); // expected-error {{the lambda may change 'v', which is passed with it [weavec-safe:call-alias]}}
  believes(v, [] {});
}
// p16, types/p08: a generic lambda, each instance of its call operator read
void visited() {
  V v = std::string("hello");
  std::visit([&v](auto &x) { // expected-error {{the lambda may change 'v', which is passed with it [weavec-safe:call-alias]}}
    if constexpr (std::is_same_v<std::decay_t<decltype(x)>, std::string>)
      v = K;
  }, v);
  long n = 0;
  std::visit([&n](const auto &x) { n += sizeof(x); }, v);
}
void genericCalled() {
  V v = std::string("hello");
  auto set = [&v](auto value) { v = value; };
  std::string &s = std::get<std::string>(v);
  set(K);
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'set', which changes 'v' [weavec-safe:stale-borrow]}}
}
// A lambda that changes nothing ends nothing; one that changes something
// else ends nothing of `v`.
void readsOnly() {
  V v = std::string("hello");
  long n = 0;
  std::string &s = std::get<std::string>(v);
  auto count = [&n, &v] { n += static_cast<long>(v.index()); };
  count();
  run([&n] { ++n; });
  s[0] = 'Z';
}
// The visitor that loops over its own container and calls what it is lent.
struct Index {
  std::map<std::string, int> items;
  void forEach(const std::function<void(const std::string &, int)> &fn) const {
    for (const auto &[key, value] : items)
      fn(key, value);
  }
};
int total(const Index &index) {
  int sum = 0;
  index.forEach([&sum](const std::string &, int value) { sum += value; });
  return sum;
}
// A lambda another lambda calls.
void nested() {
  V v = std::string("hello");
  auto flip = [&v] { v = K; };
  auto twice = [&flip] { flip(); flip(); };
  std::string &s = std::get<std::string>(v);
  twice();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call of 'twice', which changes 'v' [weavec-safe:stale-borrow]}}
}
