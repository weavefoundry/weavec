// RFC 0049 §3.1, §3.3, §3.4: a kept parameter is WEAVEC_ESCAPES (C1,
// certain), on every declaration; `this`, after the parameter list and its
// qualifiers; a pointer kept in a member of a class of plain values makes
// the member WEAVEC_BORROWED first (D5); a global that never changes is
// `const` (C4); declarations that disagree are made to agree (C2); a class
// changed through pointers kept elsewhere is a choice (WEAVEC_ALIASED), with
// no fix-it.
// RUN: not %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s
#include <cstddef>
#include <memory>
#include <vector>
#include <weavec.h>

struct Counter {
  long hits = 0;
};
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &c);
  Registry *self = nullptr;
  void into(Registry &other) noexcept { other.self = this; }
  void own() { self = this; }
};
void Registry::add(Counter &c) { counters.push_back(&c); }
// CHECK: error: 'this' is kept by 'other.self', and 'into' only borrows it [weavec-safe:escape]
// CHECK: note: fix (certain): declare it 'void into(Registry &other) noexcept WEAVEC_ESCAPES'
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:38-{{[0-9]+}}:38}:" WEAVEC_ESCAPES"
// CHECK: error: 'this' is kept by 'self', and 'own' only borrows it [weavec-safe:escape]
// CHECK: note: fix (certain): declare it 'void own() WEAVEC_ESCAPES'
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:13-{{[0-9]+}}:13}:" WEAVEC_ESCAPES"
// CHECK: error: the address of 'c' is kept by 'push_back', and 'add' only borrows it [weavec-safe:escape]
// CHECK: note: fix (certain): declare it 'Counter &WEAVEC_ESCAPES c', and at {{.*}}fix-escapes.cpp:{{[0-9]+}}:29
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:21-{{[0-9]+}}:21}:"WEAVEC_ESCAPES "
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:29-{{[0-9]+}}:29}:"WEAVEC_ESCAPES "

// D5: a slice is a view.
class Slice {
public:
  Slice(const char *d, std::size_t n) : data_(d), size_(n) {}

private:
  const char *data_;
  std::size_t size_;
};
// CHECK: error: the address of 'd' is kept by 'data_', and 'Slice' only borrows it [weavec-safe:escape]
// CHECK: note: fix (likely): declare the member 'const char *WEAVEC_BORROWED data_', a view's; or (certain) the parameter 'const char *WEAVEC_ESCAPES d' if 'Slice' objects are kept
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:15-{{[0-9]+}}:15}:"WEAVEC_BORROWED "
// CHECK-NOT: fix-it:{{.*}}WEAVEC_ESCAPES

// C4: never assigned, so `const`; one that is assigned has no fix.
static const char *greeting = "hello";
static int calls = 0;
const char *hello() { return greeting; }
int called() { return ++calls; }
// CHECK: error: every thread can reach the global 'greeting', and 'const char *' is not safe to share [weavec-safe:thread-share]
// CHECK: note: fix (certain): declare it 'static const char *const greeting'
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:20-{{[0-9]+}}:20}:"const "
// CHECK: error: every thread can reach the global 'calls', and 'int' is not safe to share [weavec-safe:thread-share]
// CHECK-NOT: fix (
// CHECK: note: make it 'thread_local'

// C2: what an earlier declaration states, written on the later one.
int first(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n);
int first(const int *values, std::size_t count) { return count != 0 ? values[0] : 0; }
// CHECK: error: this declaration of 'first' gives its pointers other bounds than an earlier one [weavec-safe:bad-annotation]
// CHECK: note: fix (certain): write WEAVEC_COUNTED_BY(count) on 'values'
// CHECK: fix-it:"{{.*}}fix-escapes.cpp":{{[{][0-9]+}}:22-{{[0-9]+}}:22}:"WEAVEC_COUNTED_BY(count) "

// A choice: WEAVEC_ALIASED is the class's author's to decide.
struct Node {
  std::unique_ptr<Node> child;
  Node *parent = nullptr;
  void grow() { parent->child.reset(); }
};
// CHECK: error: 'this->parent->child' is reached through a pointer kept in memory, and 'reset' may change what it owns [weavec-safe:owner-path]
// CHECK: note: choice: declare 'Node' WEAVEC_ALIASED if its objects are changed through pointers other objects keep
// CHECK-NOT: fix-it
// CHECK: errors carry a fix
