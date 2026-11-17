// RFC 0048 §4 (D3): a safe C++ function keeps the address of what a pointer
// or reference parameter, or `this`, refers to only if it says so:
// WEAVEC_ESCAPES on the parameter (after its `*` or `&`, or after a member
// function's parameter list for `this`), or the class is WEAVEC_ALIASED. A
// local whose address is kept is placed in typed memory, with no error; a
// temporary, a parameter taken by value and a `thread_local` are not.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// Code that is not safe is not checked.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -w -DGUARDED %s
#include <vector>
#include <weavec.h>
#ifdef GUARDED
#define CHECKED WEAVEC_GUARDED
#else
#define CHECKED
#endif

struct Counter {
  long hits = 0;
};

struct Registry {
  std::vector<Counter *> counters;
  Counter *last = nullptr;
  CHECKED void add(Counter &c) {
    counters.push_back(&c); // expected-error {{the address of 'c' is kept by 'push_back', and 'add' only borrows it [weavec-safe:escape]}}
  }
  CHECKED void point(Counter *c) {
    last = c; // expected-error {{the address of 'c' is kept by 'last', and 'point' only borrows it [weavec-safe:escape]}}
  }
  CHECKED void keep(Counter &WEAVEC_ESCAPES c) { counters.push_back(&c); }
  CHECKED void keepPointer(Counter *WEAVEC_ESCAPES c) { last = c; }
  const Counter *seen = nullptr;
  CHECKED void see(const Counter &WEAVEC_ESCAPES c) { seen = &c; }
  // A parameter passed on to one that keeps keeps too.
  CHECKED void forward(Counter &c) {
    keep(c); // expected-error {{the address of 'c' is kept by 'keep', and 'forward' only borrows it [weavec-safe:escape]}}
  }
  CHECKED void forwardKept(Counter &WEAVEC_ESCAPES c) { keep(c); }
};

struct Listener {
  Registry *registry = nullptr;
  Listener *self = nullptr;
  CHECKED void enroll() {
    self = this; // expected-error {{'this' is kept by 'self', and 'enroll' only borrows it [weavec-safe:escape]}}
  }
  CHECKED void enrollKept() WEAVEC_ESCAPES { self = this; }
};

// An aliased class's objects are kept by declaration.
struct WEAVEC_ALIASED Node {
  Node *parent = nullptr;
  Node *child = nullptr;
  CHECKED void adopt(Node &other) {
    other.parent = this;
    child = &other;
  }
};

// A local whose address is kept lives in typed memory: no error.
CHECKED long locals(Registry &registry) {
  Counter mine;
  registry.keep(mine);
  Node root;
  Node leaf;
  root.adopt(leaf);
  return mine.hits;
}

CHECKED void temporary(Registry &registry) {
  registry.see(Counter()); // expected-error {{a temporary is kept by 'see'}}
}

CHECKED void byValue(Registry &registry, Counter c) {
  registry.keep(c); // expected-error {{the address of 'c', a parameter taken by value, is kept by 'keep'}}
}

thread_local Counter perThread;
CHECKED void threadLocal(Registry &registry) {
  registry.keep(perThread); // expected-error {{'perThread' is thread_local, and its address is kept by 'keep'}}
}

// A member constructed from an argument that it keeps keeps it too.
struct Wrap {
  Counter *held;
  CHECKED explicit Wrap(Counter &WEAVEC_ESCAPES c) : held(&c) {}
};
struct Outer {
  Wrap wrap;
  CHECKED explicit Outer(Counter &c) : wrap(c) {} // expected-error {{the address of 'c' is kept by 'Wrap', and 'Outer' only borrows it [weavec-safe:escape]}}
};
struct OuterKept {
  Wrap wrap;
  CHECKED explicit OuterKept(Counter &WEAVEC_ESCAPES c) : wrap(c) {}
};

// A constructor that keeps its object (a small buffer's own pointer) says
// so; so does each constructor that makes such a member, and its locals,
// even of a class whose constructor is implicit, are placed.
struct Small {
  int pool[4] = {};
  int *mem;
  CHECKED Small() WEAVEC_ESCAPES : mem(pool) {}
};
struct Unsaid {
  int *mem;
  int pool[4] = {};
  CHECKED Unsaid() : mem(pool) {} // expected-error {{'this' is kept by 'mem', and 'Unsaid' only borrows it [weavec-safe:escape]}}
};
struct HoldsSmall {
  Small inner;
  CHECKED HoldsSmall() {} // expected-error {{'this' is kept by 'Small', and 'HoldsSmall' only borrows it [weavec-safe:escape]}}
};
struct HoldsSmallKept {
  Small inner;
  CHECKED HoldsSmallKept() WEAVEC_ESCAPES {}
};
struct HoldsSmallImplicitly {
  Small inner;
};
CHECKED int smallLocals() {
  Small a;
  HoldsSmallImplicitly b;
  HoldsSmallKept c;
  return *a.mem + *b.inner.mem + *c.inner.mem;
}
CHECKED int readSmall(const Small &small) { return *small.mem; }
CHECKED int smallTemporary() {
  return readSmall(Small()); // expected-error {{a temporary is kept by its own constructor}}
}
// A temporary a local reference extends is no local either.
CHECKED int smallExtended() {
  const Small &small = Small(); // expected-error {{a temporary is kept by its own constructor}}
  return *small.mem;
}
// A member's default initializer is each constructor's code that does not
// name the member; an implicit constructor's keeps its object.
struct ByDefault {
  int pool[4] = {};
  int *mem = pool;
  CHECKED ByDefault() {} // expected-error {{'this' is kept by 'mem', and 'ByDefault' only borrows it [weavec-safe:escape]}}
  CHECKED explicit ByDefault(int *WEAVEC_ESCAPES given) : mem(given) {}
};
struct ByDefaultImplicitly {
  int pool[4] = {};
  int *mem = pool;
};
CHECKED int byDefault() {
  ByDefaultImplicitly made;
  return *made.mem;
}

// A view's member (WEAVEC_BORROWED) holds a borrow the view rules follow:
// storing in it keeps nothing.
struct Cursor {
  const char *WEAVEC_BORROWED at;
  CHECKED explicit Cursor(const char *text) : at(text) {}
  CHECKED void reset(const char *text) { at = text; }
};
CHECKED char first(const char *text) {
  Cursor cursor(text);
  cursor.reset(text);
  return *cursor.at;
}

// A template's own parameter type is a value the template keeps, as a
// container does; its caller passes what may be kept.
template <class T> struct Stack {
  T items[4] = {};
  unsigned size = 0;
  CHECKED void push(T item) { items[size++ % 4] = item; }
};
CHECKED void pushed(Stack<Counter *> &stack, Counter &c, Counter &WEAVEC_ESCAPES d) {
  stack.push(&c); // expected-error {{the address of 'c' is kept by 'push', and 'pushed' only borrows it [weavec-safe:escape]}}
  stack.push(&d);
}
