// RFC 0052 §5.3 (D6): a callable that is kept (in a member, an element, a
// `static`, by a parameter that keeps it) captures no reference, no `this`
// and no pointer or view; one a local the function made keeps is one the
// function sees. The round of 2026-11-21's `lifetimes/p30`.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using V = std::variant<std::string, long>;
constexpr long K = 0x1111111111111111L;

// p30: a member's default initializer
struct Self {
  V v = std::string("hello");
  std::function<void()> f = [this] { v = K; }; // expected-error {{the callable that initializes 'f' is kept by it and captures 'this' [weavec-safe:escape]}}
};
struct Button {
  std::function<void()> onClick;
  void set(std::function<void()> f) { onClick = std::move(f); }
};
struct Panel : std::enable_shared_from_this<Panel> {
  Button button;
  int clicks = 0;
  void wire() {
    button.set([this] { ++clicks; }); // expected-error {{the lambda is kept by what 'set' keeps and captures 'this' [weavec-safe:escape]}}
    std::weak_ptr<Panel> weak = weak_from_this();
    button.set([weak] {
      if (auto self = weak.lock())
        ++self->clicks;
    });
  }
};
struct Queue {
  std::vector<std::function<void()>> tasks;
  void add(int *counter) {
    tasks.push_back([counter] { ++*counter; }); // expected-error {{the lambda is kept by 'this->tasks' and captures 'counter', a pointer [weavec-safe:escape]}}
    auto shared = std::make_shared<int>(0);
    tasks.push_back([shared] { ++*shared; });
  }
  void runAll() {
    for (auto &task : tasks)
      task();
  }
};
void kept() {
  thread_local std::function<int()> counter = [] { return 1; };
  int n = 0;
  thread_local std::function<int()> local = [&n] { return n; }; // expected-error {{the lambda is kept by the static 'local' and captures 'n' by reference [weavec-safe:escape]}}
  (void)counter;
}
// A local made with a callable it keeps keeps one the function sees.
struct Clock {
  std::function<long()> now;
  explicit Clock(std::function<long()> f) : now(std::move(f)) {}
  long tick() const { return now(); }
};
long clocked() {
  long time = 0;
  Clock clock([&time] { return time; });
  time = 5;
  return clock.tick();
}
void clockChanges() {
  V v = std::string("hello");
  Clock clock([&v] {
    v = K;
    return 0L;
  });
  std::string &s = std::get<std::string>(v);
  (void)clock.tick();
  s[0] = 'Z'; // expected-error {{'s' was taken from 'v' before the call to 'tick', which may call the lambda, which changes 'v' [weavec-safe:stale-borrow]}}
}
