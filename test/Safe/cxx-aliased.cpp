// RFC 0047 §5: an object is changed through a pointer kept in memory or a
// `std::shared_ptr` only if its class is WEAVEC_ALIASED (W), a shareable
// class that locks its members, or a library type with no storage that
// changes type in place; an aliased class owns none of that storage (K).
// RUN: %weavec_cxx -std=c++20 %cxx_threads -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <any>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include <weavec.h>

// The hole RFC 0044 left: a variant overwritten through a second handle
// while a reference into it lives.
struct Doc {
  std::variant<std::vector<long>, std::string> data;
};
void clobber(const std::shared_ptr<Doc> &d) {
  d->data = std::string("A"); // expected-error {{'d->data' is reached through a 'std::shared_ptr', and 'operator=' may change what it owns [weavec-safe:owner-path]}}
}
long read(const std::shared_ptr<Doc> &a, const std::shared_ptr<Doc> &b) {
  const std::vector<long> &v = std::get<std::vector<long>>(a->data);
  clobber(b);
  return v.empty() ? -1 : v[0];
}
void replace(std::shared_ptr<Doc> &d) { d = std::make_shared<Doc>(); }

struct Counter {
  std::vector<int> hits;
  void add(int v) { hits.push_back(v); }
};
void bump(const std::shared_ptr<Counter> &c) {
  c->add(1); // expected-error {{'c' is reached through a 'std::shared_ptr', and 'add' may change what it owns [weavec-safe:owner-path]}}
}
int peek(const std::shared_ptr<Counter> &c) { return int(c->hits.size()); }

class WEAVEC_ALIASED Env {
public:
  void open(const std::string &name) { names_.push_back(name); }

private:
  std::vector<std::string> names_;
};
class WEAVEC_ALIASED PosixEnv : public Env {};
struct User {
  Env *env = nullptr;
  std::shared_ptr<Env> shared;
  PosixEnv *posix = nullptr;
  void go() {
    env->open("a");
    shared->open("b");
    posix->open("c");
  }
};

// A class that locks its members is changed only under its lock.
class Tally {
public:
  void add(int v) {
    std::lock_guard<std::mutex> hold(mu_);
    values_.push_back(v);
  }

private:
  std::mutex mu_;
  std::vector<int> values_;
};
void tally(const std::shared_ptr<Tally> &t) { t->add(1); }

struct Holder {
  std::vector<int> *numbers = nullptr;
  std::string *text = nullptr;
  void push() { numbers->push_back(1); }
  void append() {
    text->append("x"); // expected-error {{'this->text' is reached through a pointer kept in memory, and 'append' may change what it owns [weavec-safe:owner-path]}}
  }
};

// K: what an aliased class may own.
struct WEAVEC_ALIASED Tagged {
  std::variant<int, std::string> v; // expected-error {{'Tagged' is WEAVEC_ALIASED and owns 'v', whose contents change type in place [weavec-safe:owner-path]}}
};
struct WEAVEC_ALIASED Hook {
  std::function<void()> callback; // expected-error {{'Hook' is WEAVEC_ALIASED and owns 'callback', whose contents change type in place [weavec-safe:owner-path]}}
};
struct WEAVEC_ALIASED Bag {
  std::vector<std::any> items; // expected-error {{'Bag' is WEAVEC_ALIASED and owns 'items', whose contents change type in place [weavec-safe:owner-path]}}
};
struct Shape {
  virtual ~Shape() = default;
};
struct WEAVEC_ALIASED Scene {
  std::vector<std::unique_ptr<Shape>> shapes; // expected-error {{'Scene' is WEAVEC_ALIASED and owns 'shapes', whose contents change type in place [weavec-safe:owner-path]}}
};
struct WEAVEC_ALIASED Widget {
  virtual ~Widget() = default;
};
struct WEAVEC_ALIASED Window {
  std::vector<std::string> titles;
  std::unique_ptr<Env> env;
  std::vector<std::unique_ptr<Widget>> widgets;
  std::shared_ptr<Doc> doc;
};
