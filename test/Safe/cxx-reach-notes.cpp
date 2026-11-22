// RFC 0052's messages under existing rules, with their notes: a call that
// reaches through a view, a call that may run a lambda, a callable beside
// what it changes, a kept callable, a value that holds a borrow, and E8's
// `lifetimebound`.
// RUN: not %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -fno-caret-diagnostics %s 2>&1 | FileCheck %s
#include <functional>
#include <string>
#include <variant>

using V = std::variant<std::string, long>;
struct M {
  V &v;
  void flip() const { v = 1L; }
};
void reached(V &v) {
  const M m{v};
  std::string &s = std::get<std::string>(v);
  m.flip();
  // CHECK: cxx-reach-notes.cpp:[[@LINE+2]]:{{[0-9]+}}: error: 's' points into 'v', which the call to 'flip' may reach through its object [weavec-safe:stale-borrow]
  // CHECK-NEXT: cxx-reach-notes.cpp:[[@LINE-2]]:{{[0-9]+}}: note: called here; take 's' again after the call, or give the call a view of what is 'const' ('std::span<const T>', 'const T *')
  s[0] = 'Z';
}
void run(const std::function<void()> &cb) { cb(); }
void runs(V &v) {
  std::string &s = std::get<std::string>(v);
  run([&v] { v = 1L; });
  // CHECK: cxx-reach-notes.cpp:[[@LINE+2]]:{{[0-9]+}}: error: 's' was taken from 'v' before the call to 'run', which may call the lambda, which changes 'v' [weavec-safe:stale-borrow]
  // CHECK-NEXT: cxx-reach-notes.cpp:[[@LINE-2]]:{{[0-9]+}}: note: called here; take 's' again after the call, or let the lambda return what to change and change 'v' after the call
  s[0] = 'Z';
}
void both(V &v, const std::function<void()> &cb);
void alias(V &v) {
  // CHECK: cxx-reach-notes.cpp:[[@LINE+2]]:{{[0-9]+}}: error: the lambda may change 'v', which is passed with it [weavec-safe:call-alias]
  // CHECK-NEXT: cxx-reach-notes.cpp:[[@LINE+1]]:{{[0-9]+}}: note: collect the changes in a local and apply them after the call
  both(v, [&v] { v = 1L; });
}
struct Button {
  std::function<void()> onClick;
};
struct Panel {
  Button button;
  int clicks = 0;
  void wire() {
    // CHECK: cxx-reach-notes.cpp:[[@LINE+2]]:{{[0-9]+}}: error: the lambda is kept by 'this->button.onClick' and captures 'this' [weavec-safe:escape]
    // CHECK-NEXT: cxx-reach-notes.cpp:[[@LINE+1]]:{{[0-9]+}}: note: a kept callable owns what it uses: capture by value what the body needs, a 'std::shared_ptr' or 'std::weak_ptr' to the object, or take the object as the callable's argument
    button.onClick = [this] { ++clicks; };
  }
};
struct R {
  long *p;
};
R made() {
  long x = 5;
  // CHECK: cxx-reach-notes.cpp:[[@LINE+2]]:{{[0-9]+}}: error: 'R{&x}' holds a borrow of 'x' and is returned [weavec-safe:escape]
  // CHECK-NEXT: cxx-reach-notes.cpp:[[@LINE+1]]:{{[0-9]+}}: note: return what owns its data, or take 'x' as a parameter so that the caller keeps it
  return R{&x};
}
const char &firstOf(const std::string &a, const std::string &b) { return a[0]; }
void e8(std::string &x, std::string &y) {
  const char &r = firstOf(x, y);
  y.append("z");
  // CHECK: cxx-reach-notes.cpp:[[@LINE+2]]:{{[0-9]+}}: error: 'r' points into 'y', which 'append' may have changed [weavec-safe:stale-borrow]
  // CHECK-NEXT: cxx-reach-notes.cpp:{{[0-9]+}}:{{[0-9]+}}: note: fix (likely): declare it 'const std::string &a WEAVEC_LIFETIMEBOUND'; every return of 'firstOf' gives 'a'
  (void)r;
}
