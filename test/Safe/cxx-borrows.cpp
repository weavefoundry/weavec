// RFC 0044 §5, §6, §8, §9: Safe C++'s flow rules on owners and borrows, each
// with accepted and rejected forms: owner-moved, stale-borrow, call-alias
// and owner-path.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct Item {
  int v = 0;
  std::string name;
  std::vector<int> tags;
};
std::string make(int n);
void sink(std::unique_ptr<Item> p);
void grow(std::vector<int> &v);
void look(const std::vector<int> &v);
void touch(std::string &s);

// owner-moved
int movedPointer() {
  auto p = std::make_unique<Item>();
  sink(std::move(p));
  return p->v; // expected-error {{'p' is used after it was moved [weavec-safe:owner-moved]}}
}
int movedOnOnePath(std::vector<int> v, bool flag, std::vector<std::vector<int>> &all) {
  if (flag)
    all.push_back(std::move(v));
  return int(v.size()); // expected-error {{'v' is used after it was moved [weavec-safe:owner-moved]}}
}
int movedInTry(std::unique_ptr<int> p, std::vector<std::unique_ptr<int>> &all) {
  try {
    all.push_back(std::move(p));
  } catch (...) {
    return *p; // expected-error {{'p' is used after it was moved [weavec-safe:owner-moved]}}
  }
  return 0;
}
int assignedAgain(std::vector<int> v, std::vector<std::vector<int>> &all) {
  all.push_back(std::move(v));
  v = {1, 2};
  return int(v.size());
}
int notTaken(std::vector<int> v) {
  look(std::move(v)); // a const reference takes nothing
  return int(v.size());
}
int movedScalar(int n) {
  int m = std::move(n); // a copy
  return n + m;
}
struct Queue {
  std::vector<int> items;
  std::string label;
  std::vector<int> takeLeft() {
    return std::move(items); // expected-error {{'items' is left moved when the function returns [weavec-safe:owner-moved]}}
  }
  std::vector<int> take() { return std::exchange(items, {}); }
  std::vector<int> takeAndReset() {
    std::vector<int> out = std::move(items);
    items = {};
    return out;
  }
  std::string consume() && { return std::move(label); }
  int partlyMoved(std::vector<std::string> &all) {
    all.push_back(std::move(label));
    const int n = int(label.size()); // expected-error {{'label' is used after it was moved [weavec-safe:owner-moved]}}
    label = "";
    return n;
  }
};
Item wholeWhilePartMoved(Item item, std::vector<std::string> &all) {
  all.push_back(std::move(item.name));
  return item; // expected-error {{'item' is used while its member 'name' is moved [weavec-safe:owner-moved]}}
}
void movedByLambda(std::vector<int> v, std::vector<std::vector<int>> &all) {
  auto keep = [w = std::move(v)] { return w.size(); };
  all.push_back(v); // expected-error {{'v' is used after it was moved [weavec-safe:owner-moved]}}
  (void)keep;
}

// stale-borrow: the owner changed
int acrossPushBack(std::vector<int> &v) {
  int &r = v[0];
  v.push_back(1);
  return r; // expected-error {{'r' points into 'v', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
}
int acrossCall() {
  std::vector<int> v{1, 2, 3};
  int &r = v[0];
  grow(v);
  return r; // expected-error {{'r' was taken from 'v' before the call to 'grow', which may change what 'v' owns [weavec-safe:stale-borrow]}}
}
int acrossConstCall() {
  std::vector<int> v{1, 2, 3};
  int &r = v[0];
  look(v);
  return r;
}
int takenAgain(std::vector<int> &v) {
  int *p = &v[0];
  v.push_back(1);
  p = &v[0];
  return *p;
}
int rangeFor(std::vector<int> &v) {
  int s = 0;
  for (int &x : v) { // expected-error {{the loop over 'v' continues after 'push_back' may have changed 'v' [weavec-safe:stale-borrow]}}
    if (x > 3)
      v.push_back(x);
    s += x; // expected-error {{'x' points into 'v', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
  }
  return s;
}
int rangeForOther(std::vector<int> &v, std::vector<int> &w) {
  int s = 0;
  for (int &x : v) {
    w.push_back(x);
    s += x;
  }
  return s;
}
void elementsChange(std::vector<Item> &items) {
  for (Item &item : items) {
    item.tags.push_back(item.v); // changes an element, not the vector
    item.name += "!";
  }
}
int deeperThanElement(std::vector<Item> &items) {
  Item &item = items[0];
  int &tag = item.tags[0];
  item.tags.push_back(1);
  return tag; // expected-error {{'tag' points into 'items', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
}
int iteratorAfterErase(std::vector<int> &v) {
  auto it = v.begin();
  v.erase(v.begin());
  return *it; // expected-error {{'it' points into 'v', which 'erase' may have changed [weavec-safe:stale-borrow]}}
}
void eraseLoop(std::vector<int> &v) {
  for (auto it = v.begin(); it != v.end();) {
    if (*it % 2 != 0)
      it = v.erase(it);
    else
      ++it;
  }
}
void eraseRemove(std::vector<int> &v) {
  v.erase(std::remove_if(v.begin(), v.end(), [](int x) { return x < 0; }), v.end());
}
int afterReset() {
  auto p = std::make_unique<Item>();
  Item *raw = p.get();
  p.reset();
  return raw->v; // expected-error {{'raw' points into 'p', which 'reset' may have changed [weavec-safe:stale-borrow]}}
}
int mapErase(std::map<int, Item> &m) {
  Item &e = m[1];
  m.erase(1);
  return e.v; // expected-error {{'e' points into 'm', which 'erase' may have changed [weavec-safe:stale-borrow]}}
}
int mapInserts(std::map<int, std::string> &m) {
  std::string &a = m[1];
  std::string &b = m[2]; // operator[] inserts: it has no const overload
  return int(a.size() + b.size()); // expected-error {{'a' points into 'm', which 'operator[]' may have changed [weavec-safe:stale-borrow]}}
}
int mapAccessors(std::map<int, std::string> &m) {
  std::string &a = m.at(1);
  std::string &b = m.at(2);
  auto it = m.find(3);
  return int(a.size() + b.size()) + (it == m.end() ? 0 : 1);
}
int twoElements(std::vector<int> &v) {
  int &a = v[0];
  int &b = v[1];
  std::swap(a, b);
  return a + b;
}
int optionalReset(std::optional<std::string> &o) {
  std::string &s = *o;
  o.reset();
  return int(s.size()); // expected-error {{'s' points into 'o', which 'reset' may have changed [weavec-safe:stale-borrow]}}
}
int assignedOver(std::string &s) {
  std::string_view view = s;
  s = "another string that is long enough to move";
  return int(view.size()); // expected-error {{'view' points into 's', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
int spanAcrossCall(std::vector<int> &v) {
  std::span<int> s(v);
  grow(v);
  return s[0]; // expected-error {{'s' was taken from 'v' before the call to 'grow', which may change what 'v' owns [weavec-safe:stale-borrow]}}
}
void twiceThroughAReference(std::vector<int> &v, std::string &s) {
  v.push_back(1);
  v.push_back(2);
  s += "a";
  s += "b";
}

// stale-borrow: the owner is gone
int viewOfTemporary(int n) {
  std::string_view sv = make(n);
  return int(sv.size()); // expected-error {{'sv' points into a temporary 'std::string', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
}
int pointerOfTemporary(int n) {
  const char *p = make(n).c_str();
  return *p; // expected-error {{'p' points into a temporary 'std::string', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
}
int extended(int n) {
  const std::string &s = make(n); // the reference keeps the temporary
  return int(s.size());
}
int inOneExpression(int n) {
  return int(std::string_view(make(n)).size());
}
int innerScope() {
  const std::string *p = nullptr;
  {
    std::string s = make(1);
    p = &s;
  }
  return int(p->size()); // expected-error {{'p' points into 's', whose lifetime has ended [weavec-safe:stale-borrow]}}
}
struct Holder {
  const std::string &s;
  explicit Holder(const std::string &x) : s(x) {}
};
int holderOfTemporary(int n) {
  Holder h(make(n));
  return int(h.s.size()); // expected-error {{'h' points into a temporary 'std::string', which was destroyed at the end of its statement [weavec-safe:stale-borrow]}}
}
int afterMove(std::vector<std::string> all) {
  std::string s = make(1);
  std::string_view view = s;
  all.push_back(std::move(s));
  return int(view.size()); // expected-error {{'view' points into 's', which was moved [weavec-safe:stale-borrow]}}
}

// call-alias
void add(std::vector<std::string> &v, const std::string &s);
void both(std::vector<std::string> &a, std::vector<std::string> &b);
void containerAndElement(std::vector<std::string> &v) {
  add(v, v[0]); // expected-error {{'v[0]' points into 'v', which 'add' may change through 'v' [weavec-safe:call-alias]}}
}
void sameTwice(std::vector<std::string> &v) {
  both(v, v); // expected-error {{'v' points into 'v', which 'both' may change through 'v' [weavec-safe:call-alias]}}
}
void copyFirst(std::vector<std::string> &v, std::vector<std::string> &w) {
  const std::string first = v[0];
  add(v, first);
  add(v, w[0]);
  both(v, w);
}
void libraryHandlesIt(std::vector<std::string> &v) {
  v.push_back(v[0]);
  std::sort(v.begin(), v.end());
}
void callableChanges(std::vector<int> &v) {
  std::for_each(v.begin(), v.end(), [&](int x) { v.push_back(x); }); // expected-error {{'v.begin()' points into 'v', which the callable passed with it may change [weavec-safe:call-alias]}}
}
int callableReads(const std::vector<int> &v, std::vector<int> &out) {
  std::for_each(v.begin(), v.end(), [&](int x) { out.push_back(x); });
  return int(std::count_if(v.begin(), v.end(), [&](int x) { return x > int(v.size()); }));
}

// owner-path
struct Subject;
struct Observer {
  Subject *subject = nullptr;
  int seen = 0;
  void notify();
  void count();
};
struct Subject {
  std::vector<Observer *> observers;
  std::string name;
  int notified = 0;
  void remove(Observer *o);
  void notifyAll() {
    for (Observer *o : observers)
      o->count(); // an Observer owns nothing
  }
};
void Observer::notify() {
  subject->remove(this); // expected-error {{'this->subject' is reached through a pointer kept in memory, and 'remove' may change what it owns [weavec-safe:owner-path]}}
}
void Observer::count() {
  ++seen;
  subject->notified += 1; // a member that owns nothing
}
void rename(Observer &o) {
  o.subject->name = "renamed"; // expected-error {{'o.subject->name' is reached through a pointer kept in memory, and 'operator=' may change what it owns [weavec-safe:owner-path]}}
}
int readThrough(const Observer &o) { return int(o.subject->name.size()); }
struct Tree {
  std::vector<std::unique_ptr<Tree>> kids;
  Tree *parent = nullptr;
  std::shared_ptr<Tree> twin;
  std::string label;
  void growOwned() { kids.back()->label += "x"; }
  void growParent() {
    parent->kids.push_back(nullptr); // expected-error {{'this->parent->kids' is reached through a pointer kept in memory, and 'push_back' may change what it owns [weavec-safe:owner-path]}}
  }
  void growShared() { twin->label += "x"; } // a shared object may be changed
  void passDown(Tree &to) { to.label += label; }
};
struct Cache {
  mutable std::string last;
  std::vector<int> data;
  int get(std::size_t i) const {
    last = "hit"; // expected-error {{'this->last' is changed in a member function that is 'const' or has a 'const' overload, which only gives access [weavec-safe:owner-path]}}
    return data[i];
  }
  int &slot(std::size_t i) {
    data.resize(i + 1); // expected-error {{'this->data' is changed in a member function that is 'const' or has a 'const' overload, which only gives access [weavec-safe:owner-path]}}
    return data[i];
  }
  const int &slot(std::size_t i) const { return data[i]; }
};
