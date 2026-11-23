// RFC 0053 §1, §3, §5, §7: calls the rounds' programs do not make. A
// constructor of the program given a callable may call it, as a function
// may; a copy of a callable runs nothing. A call through a pointer to a
// member function changes its object as a call of the member function does.
// A copy or a move of a local that keeps a borrow holds that borrow. A
// position in a local keeper of callables brings it no borrow. D6 and D7 are
// answered alike for a recursive class whichever of its types is asked
// first.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <weavec.h>

// §1: a constructor is a callee like any other.
namespace constructor_calls {
struct Once {
  template <class F> explicit Once(F f) { f(); }
};
int hole(std::vector<int> &v) {
  int &r = v[0];
  Once once([&] { v.clear(); });
  return r; // expected-error {{'r' was taken from 'v' before the call to 'Once', which may call the lambda, which changes 'v' [weavec-safe:stale-borrow]}}
}
// A copy of a callable runs nothing.
int copies(std::vector<int> &v) {
  auto clear = [&v] { v.clear(); };
  int &r = v[0];
  auto again = clear;
  auto moved = std::move(again);
  return r;
}
} // namespace constructor_calls

// §1: through `.*` and `->*` the object is changed as by the member function.
namespace member_pointer_changes {
struct Bag {
  std::vector<int> items;
  void drop() { items.clear(); }
  std::size_t count() const { return items.size(); }
};
int hole(Bag &bag) {
  auto drop = &Bag::drop;
  int &r = bag.items[0];
  (bag.*drop)();
  return r; // expected-error {{'r' points into 'bag', which a call may have changed [weavec-safe:stale-borrow]}}
}
int arrow(Bag &bag) {
  auto drop = &Bag::drop;
  Bag *at = &bag;
  int &r = bag.items[0];
  (at->*drop)();
  return r; // expected-error {{'r' points into 'bag', which a call may have changed [weavec-safe:stale-borrow]}}
}
// A member function that is `const` changes nothing of a class with no
// `mutable` owner.
int reads(Bag &bag) {
  auto count = &Bag::count;
  int &r = bag.items[0];
  const std::size_t n = (bag.*count)();
  return r + static_cast<int>(n);
}
struct Memo {
  mutable std::vector<int> seen;
  void note() const;
};
int shallow(Memo &memo) {
  auto note = &Memo::note;
  const int &r = memo.seen[0];
  (memo.*note)();
  return r; // expected-error {{'r' points into 'memo', which a call may have changed [weavec-safe:stale-borrow]}}
}
} // namespace member_pointer_changes

// §5: a local assigned, or made from, a local that keeps a borrow keeps it.
namespace keeper_copied {
std::shared_ptr<int> assigned() {
  auto owner = std::make_shared<int>(1);
  int x = 5;
  std::shared_ptr<int> a(owner, &x);
  std::shared_ptr<int> b(owner, &x);
  b = a;
  return b; // expected-error {{'b' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
}
std::shared_ptr<int> first() {
  auto owner = std::make_shared<int>(1);
  int x = 5;
  std::shared_ptr<int> a(owner, &x);
  std::shared_ptr<int> b;
  b = a;
  return b; // expected-error {{'b' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
}
std::shared_ptr<int> copied() {
  auto owner = std::make_shared<int>(1);
  int x = 5;
  std::shared_ptr<int> a(owner, &x);
  std::shared_ptr<int> b = a;
  return b; // expected-error {{'b' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
}
std::shared_ptr<int> moved() {
  auto owner = std::make_shared<int>(1);
  int x = 5;
  std::shared_ptr<int> a(owner, &x);
  std::shared_ptr<int> b;
  b = std::move(a);
  return b; // expected-error {{'b' holds a borrow of 'x' and is returned [weavec-safe:escape]}}
}
// Assigned an owner, it holds the borrow no longer.
std::shared_ptr<int> released() {
  auto owner = std::make_shared<int>(1);
  int x = 5;
  std::shared_ptr<int> b(owner, &x);
  b = owner;
  return b;
}
} // namespace keeper_copied

// §3: what points into the keeper itself is not kept by it.
namespace keeper_position {
void erases() {
  std::vector<std::function<void()>> callbacks;
  callbacks.push_back([] {});
  auto it = callbacks.begin();
  callbacks.erase(it);
}
void inserts(const std::function<void()> &callback) {
  std::vector<std::function<void()>> callbacks;
  callbacks.insert(callbacks.begin(), callback);
}
void clears(const std::function<void()> &callback) {
  std::vector<std::function<void()>> callbacks;
  callbacks.push_back(callback);
  callbacks.erase(callbacks.begin(), callbacks.end());
}
// Positions in another local are what it holds.
struct Flip {
  std::vector<int> &v;
  void operator()() const { v.clear(); }
};
void hole(std::vector<int> &v) {
  std::array<Flip, 1> flips{Flip{v}};
  std::vector<std::function<void()>> callbacks;
  callbacks.insert(callbacks.end(), flips.begin(), flips.end()); // expected-error {{'flips.begin()' is kept by 'callbacks' and holds a borrow of 'flips' [weavec-safe:escape]}} expected-error {{'flips.end()' is kept by 'callbacks' and holds a borrow of 'flips' [weavec-safe:escape]}}
}
} // namespace keeper_position

// §7 (D7): the class asked before the library's class of it.
namespace recursive_mutable {
struct Node {
  std::vector<Node> kids;
  mutable std::vector<int> cache;
};
void peek(const Node &node);
void warm(const std::vector<Node> &nodes);
int one(Node &node) {
  const int &a = node.cache[0];
  peek(node);
  return a; // expected-error {{'a' was taken from 'node' before the call to 'peek', which may change what 'node' owns [weavec-safe:stale-borrow]}}
}
int many(std::vector<Node> &nodes) {
  const int &b = nodes[0].cache[0];
  warm(nodes);
  return b; // expected-error {{'b' was taken from 'nodes' before the call to 'warm', which may change what 'nodes' owns [weavec-safe:stale-borrow]}}
}
} // namespace recursive_mutable

// §7 (D6): the same, for a destructor the program wrote.
namespace recursive_program {
thread_local std::vector<int> table;
struct Log {
  ~Log() { table.clear(); }
};
struct Tree {
  std::vector<Tree> kids;
  Log log;
};
int one() {
  const int &a = table[0];
  { Tree tree; }
  return a; // expected-error {{'a' points into the global 'table', which the call to 'Tree' may change [weavec-safe:stale-borrow]}}
}
int many(std::vector<Tree> &trees) {
  const int &b = table[0];
  trees.clear();
  return b; // expected-error {{'b' points into the global 'table', which the call to 'clear' may change [weavec-safe:stale-borrow]}}
}
} // namespace recursive_program
