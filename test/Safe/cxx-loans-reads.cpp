// RFC 0053: what the loans of callables, containers and polymorphic objects
// leave correct. A capture the lambda's body only reads ends nothing at its
// call; one it passes to a function that may change it does (an accepted
// rejection, whose fix is a capture by reference to const). A reference to a
// global a function returns survives a call; one into a global is not
// returned. A function that returns a value holding a borrow its type does
// not show marks the parameter `lifetimebound`, and its callers see it. A
// function that keeps a borrow of a parameter in what another refers to
// declares it WEAVEC_ESCAPES, and its callers see that too.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <weavec.h>

using V = std::variant<std::string, long>;

void log(const V &v) { std::printf("%zu\n", v.index()); }
void touch(V &v) { v = 1L; }

// The body only reads 'v': its call ends nothing.
std::size_t reads(V &v) {
  auto print = [&v] { log(v); };
  std::string &s = std::get<std::string>(v);
  print();
  return s.size();
}
// It passes 'v' to a function that may change it.
std::size_t passes(V &v) {
  auto change = [&v] { touch(v); };
  std::string &s = std::get<std::string>(v);
  change();
  return s.size(); // expected-error {{'s' was taken from 'v' before the call of 'change', which changes 'v' [weavec-safe:stale-borrow]}}
}
// A capture by reference to const is read.
std::size_t constant(V &v) {
  auto print = [&w = std::as_const(v)] { log(w); };
  std::string &s = std::get<std::string>(v);
  print();
  return s.size();
}
// A binder of a function that only reads, by `std::cref`, reads.
std::size_t bound(V &v) {
  auto print = std::bind(log, std::cref(v));
  std::string &s = std::get<std::string>(v);
  print();
  return s.size();
}

// A reference to a global, which a call may assign but not destroy.
struct Counter {
  long count = 0;
  void bump() { ++count; }
};
thread_local Counter counter;
Counter &shared() { return counter; }
long bumps() {
  Counter &c = shared();
  c.bump();
  c.bump();
  return c.count;
}
// One into a global is not returned.
thread_local std::vector<long> history;
long &latest() {
  return history.back(); // expected-error {{what is returned points into the global 'history', which a call of the program may change [weavec-safe:escape]}} expected-note {{return the global itself, or a copy of what is needed}}
}

// A factory that says what its result borrows.
struct Task {
  virtual void run() = 0;
  virtual ~Task() = default;
};
struct Reset final : Task {
  V &v;
  explicit Reset(V &v) : v(v) {}
  void run() override { v = 1L; }
};
std::unique_ptr<Task> task(V &v WEAVEC_LIFETIMEBOUND) {
  return std::make_unique<Reset>(v);
}
std::size_t runs(V &v) {
  std::unique_ptr<Task> t = task(v);
  std::string &s = std::get<std::string>(v);
  t->run(); // expected-note {{called here; take 's' again after the call, or give the call a view of what is 'const' ('std::span<const T>', 'const T *'); or call through a 'final' class, whose override the rules see}}
  return s.size(); // expected-error {{'s' points into 'v', which the virtual call to 'run' may reach through 't' [weavec-safe:stale-borrow]}}
}

// An out parameter a function fills with a borrow of what it says it keeps.
void find(V &WEAVEC_ESCAPES v,
          std::optional<std::reference_wrapper<std::string>> &found) {
  if (std::holds_alternative<std::string>(v))
    found = std::ref(std::get<std::string>(v));
}
std::size_t found() {
  V v = std::string("word");
  std::optional<std::reference_wrapper<std::string>> word;
  find(v, word);
  v = 1L;
  return word ? word->get().size() : 0; // expected-error {{'word' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}

// One that does not say it: the callee is told.
std::unique_ptr<Task> untold(V &v) {
  return std::make_unique<Reset>(v); // expected-error {{what is returned holds a borrow of 'v', which its type 'std::unique_ptr<Task>' does not show [weavec-safe:escape]}} expected-note {{mark the parameter WEAVEC_LIFETIMEBOUND, so that callers see what the result borrows; or return a type whose members show it}}
}
