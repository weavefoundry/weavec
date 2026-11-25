---
title: Write safe C++
description: Compile a C++ unit with weavec-c++ -fweavec-safe so that a use after std::move, a reference kept across push_back, a string_view of a temporary, a view that outlives its buffer and a data race stop the compile, with errors you or an agent can act on.
---

`weavec-c++` guards every access of the C++ it compiles, unchanged, and turns the hardened standard library's checks into reports. A **safe C++ function** is also checked when it compiles, against a few rules that are local to the function, as Rust's borrow checker checks safe Rust. The rules reject; they prove nothing. What a safe unit gains at run time comes from its heap, not from the rules: in the fast mode, the default, a block is reused only by its own type, so the accesses a reference or the standard library's own checks already cover need no guard (see [fast and checked builds](#fast-and-checked-builds)).

Safe C++ is ordinary modern C++ over the standard library you already use. It has no new syntax and no second library. Code written against the rules compiles with any C++ compiler.

## Making functions safe

| How                                                        | Effect                                                                                                             |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| `-fweavec-safe` on `weavec-c++`'s command line             | every function the unit defines in its own files is safe, with its lambdas and the instantiations of its templates |
| `WEAVEC_SAFE` before a function, or after `struct`/`class` | that function, or every member function the class defines, is safe in a unit without the flag                      |
| `WEAVEC_GUARDED` in the same places                        | that function or class stays guarded in a safe unit                                                                |
| `WEAVEC_UNSAFE`, `no_sanitize("address")`                  | that function stays unsafe (unguarded), as before                                                                  |

Functions from system headers (the standard library, the SDK, anything behind `-isystem`) are never checked: put third-party headers behind `-isystem`. A header of your own is checked by each safe unit that includes it, so a project converts a file at a time. C++17 or later.

```console
$ weavec-c++ -std=c++20 -fsyntax-only -fweavec-safe grow.cpp
grow.cpp:6:10: error: 'first' points into 'items', which 'push_back' may have changed [weavec-safe:stale-borrow]
grow.cpp:5:9: note: changed here; take 'first' again after this, or keep an index instead of a reference
grow.cpp:6:10: note: 'weavec-c++ --weavec-explain stale-borrow' explains this rule
```

`weavec-c++ --weavec-explain safe-cxx` prints the model on one page, `--weavec-explain <rule>` a rule with a rejected example and its rewrite, and `--weavec-explain all` everything: the brief to give an agent before it writes or converts code. The [rules reference](/reference/rules/) is the same text.

When the compiler can write what an error asks for, its note says the text and where, as a fix-it: `note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(size_) data_'; 'size_' is its count in the call to 'memcmp' at line 19`. `weavec-c++ --weavec-fix -- <the compile command>` applies every such fix, runs the rules again and repeats until nothing changes ([`fix`](/reference/rules/#fix), RFC 0049); [Convert C++ in place](/guides/convert-cxx/) uses it over a whole project.

## Owners, borrows, views and references

- An **owner** is an object: a local, a member, an element of a container, what a `std::unique_ptr` or a `std::optional` holds. Destructors release, so safe C++ writes no `new` and no `delete` (`raw-owner`).
- A **borrow** is a reference, a pointer, an iterator, a `std::string_view`, a `std::span` or a lambda that captures by reference, kept in a local or passed down a call. It ends when what it points into is destroyed, moved, assigned or changed (`stale-borrow`).
- A **view** is an object whose type holds a borrow: a struct with a `std::string_view`, a reference or a raw pointer member, a `std::vector<std::string_view>`, a `std::function` made from a capturing lambda, a `std::initializer_list`, a range adaptor. A value made from a borrow is a view of what it was made from when one of its members could point there (`R{&x}`, `std::views::all(v)`, a shared pointer made by the aliasing constructor). It is not returned or stored past what it points into (`escape`).
- A **reference** is a raw pointer kept in a member or an element. The rules do not follow it, and the guards check it when it is used. What an object owns is not changed through one, nor through a `std::shared_ptr`, unless its class is `WEAVEC_ALIASED` (`owner-path`, [sharing](#sharing)).

A raw pointer may also carry a bound, as in Safe C: `const T *WEAVEC_COUNTED_BY(n) p` points to `n` elements, `WEAVEC_ENDED_BY(e)` to those before `e`, `WEAVEC_NULL_TERMINATED` to a string, on parameters, results and members. An array keeps its length as a pointer, a container's `data()` has its `size()`, and each access through such a pointer is compared with its bound. New code writes `std::span`; code being converted keeps its pointers and declares their bounds ([Convert C++ in place](/guides/convert-cxx/)).

A class chooses between the last two when it declares a member: `Lexer &lexer_` (or a `std::string_view`) is checked when the program compiles and ties the object to a scope; `Lexer *lexer_` may live anywhere and is checked when it runs.

### What ends a borrow

A borrow into an owner ends when the owner is destroyed, moved or assigned, when a member function that is not `const` is called on it, and when it is passed to a function through a reference or pointer that is not `const`. Two things keep this from being as strict as it sounds:

- A member function that has a `const` overload with the same parameters only gives access, and ends nothing: `operator[]`, `at`, `front`, `data`, `begin`, `end`, `find`. (`operator[]` of a `std::map` inserts, has no `const` overload, and ends borrows; use `at` or `find`.)
- Changing an _element_ ends borrows into that element's own data, not borrows of the container's elements: a loop that edits each item is fine, a loop that pushes to the vector it iterates is not.

What a call could reach is read from types ([RFC 0052](/rfcs/0052-what-a-call-may-reach/)), and the default is that it may:

- A reference or pointer a function returns points into every argument whose type can hold it, and a member function's into its object as well. `char &first(std::string &s)` returns into `s`; `std::map::find(key)` returns into the map and not into the `std::string` key, which cannot hold a map's node. When a function returns into one parameter only, write `WEAVEC_LIFETIMEBOUND` on it; `--weavec-fix` proposes it when every `return` gives a borrow of that parameter.
- `const` on a view is shallow. A `std::span<T>`, a struct that holds a `T &` or a `T *`, a lambda that captures by reference: given to a function of the program, any of them reaches what it points to, whatever the parameter's `const`, and so does a call of a member function of such a view of the program's class (`setter.clear()` where `setter` holds a reference). A `std::span<const T>` or a `const T *` reaches nothing a call could change. A change made through any borrow (`std::ref(v).get() = x`, `span[0] = x`) is a change of what it names.
- Every callable is a view, and its call reaches what its loans reach ([RFC 0053](/rfcs/0053-callables-and-containers-by-their-loans/)): a lambda wherever it is held (in a `std::pair`, a `std::optional`, an array, behind a pointer, returned by a factory), a `std::bind` or `std::bind_front` of `std::ref(v)` or of `this`, a pointer to a member function with its object, a functor of the program. It ends the borrows into what it may change where it is called, and at every call it is passed to: `std::visit`, `std::invoke`, an algorithm, a function of the program. A lambda the function wrote narrows that by its body: a capture it only reads ends nothing, and neither does a binder of views of `const` (`std::bind(print, std::cref(v))`). A function that is given a callable believes it reaches nothing its other parameters reach, so passing a callable beside an object it changes, `f(v, [&v] { … })`, is `call-alias`, as Rust rejects the closure where it is made.
- The library's containers, optionals, pairs, arrays and smart pointers of pointers or reference wrappers are views of what they were made from, however they were made: `std::vector<std::reference_wrapper<V>> refs{v}` borrows `v`, and `std::pair<std::string *, int>` returned by `find(v)` borrows into `v`.
- A virtual call that is not `final`, on an object made from a borrow (a class derived from an interface, holding a reference), reaches what the object was made from. A function that returns such an object behind a base (`std::unique_ptr<Task>`) marks the parameter it borrows `WEAVEC_LIFETIMEBOUND`, since its callers cannot see the derived class; the compiler asks for the mark.
- Code of the program may change any global that is not `const`: a call of a function of the program, a call through a pointer to a function, a constructor or destructor the program wrote. So a borrow into a global does not survive it, and a function returns no reference into one. `mutable` members that own something make `const` shallow: a `const` call on their object ends the borrows into it.

As in Rust, the fix is one of: take the borrow again after the change, keep an index, copy the value out first, or declare the callee's parameter `const`.

### Fast and checked builds

A safe unit is built one of two ways, as Rust builds a debug and a release binary; without the flag, a compile without optimization is the checked one and a compile with `-O1` and above the fast one (RFC 0048):

| `-fweavec-safe-checks=`                     | What it is for                                 | At run time                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------------------------- | ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `bounds` (the default with `-O1` and above) | the build you ship and benchmark               | safe code is checked against what it holds: allocations carry their type and a block safe code releases is reused at once by its own type; no guard on an access at a constant offset through a reference, `this`, a pointer parameter or a pointer it loaded or a project function returned, none on a string's current byte, none in the standard library's functions whose preconditions its hardening checks, a `std::string_view`'s and a `std::span`'s index and slices among them, nor on the element a range-based `for` reads; its locals in plain frames; indices compared with their bounds; iterators held in variables still guarded |
| `full` (the default without optimization)   | the build you test, and an agent iterates with | exactly a guarded unit: every guard, the quarantine and the sweep, allocation stacks, container annotations; every bug the rules miss stops with a full report                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |

The rules are the same in both, so code that compiles in one compiles in the other. A stale pointer the rules missed stops when it runs in the checked build; in the fast build it reaches memory of its own type, as a stale index does in Rust, and a report of it ends with `weavec: note: built for speed; rebuild with -fweavec-safe-checks=full …`. The one error that rests on the rules alone in the fast build is a borrow into storage that changes type in place, used after the change: a reference into the alternative of a `std::variant` after the variant was assigned another alternative. `stale-borrow` rejects it in the function, and `escape` keeps a pointer into such storage out of members:

```cpp
struct Holder {
  std::string *name = nullptr;
  void keep(std::variant<std::string, int> &v) {
    name = &std::get<std::string>(v);   // error: a pointer into 'v', whose contents
  }                                     // change type, is stored in 'this->name'
};
```

Keep the index or the key, and look the value up when it is used. `weavec-cc --weavec-audit` lists the library functions a fast unit runs without guards and the allocations it could not type.

A pointer and a length are a `std::string_view` or a `std::span` in safe C++, and the fast build checks them once: the library compares an index with the view's size, as Rust's slices do, and nothing reads the shadow again ([RFC 0050](/rfcs/0050-checked-once/)). A view made from a pointer and a count is checked where it is made, so a view one element too long stops there. A range-based `for` reads its elements unchecked; an iterator kept in a variable and stepped by hand is guarded:

```cpp
std::size_t skipDigits(std::string_view text, std::size_t at) {
  while (at < text.size() && text[at] >= '0' && text[at] <= '9')   // no guard
    ++at;
  return at;
}
```

### Pointers a function keeps

A function that stores the address of what a parameter refers to, of `this`, or of a local in a member, a container or a global says so ([`escape`](/reference/rules/#escape), RFC 0048): `WEAVEC_ESCAPES` after the parameter's `*` or `&`, or after the parameter list for `this`. An object of a `WEAVEC_ALIASED` class is kept by declaration. A local whose address is kept, and a local of an aliased class, lives in typed memory for its scope, so a pointer left behind by a bug reaches memory of the local's type in the fast build and stops with a report in the checked one, never a frame that returned. A position is not a pointer to one object: an end is `WEAVEC_COUNTED_BY(0)`, a cursor `WEAVEC_ENDED_BY(limit)`, and `&p[i]` an element ([`call-args`](/reference/rules/#call-args)).

```cpp
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &WEAVEC_ESCAPES c) { counters.push_back(&c); }
};
long count(Registry &registry) {
  Counter mine;                 // its address is kept: a block of its type
  registry.add(mine);
  return mine.hits;
}
```

### What safe code can rely on

A safe unit has no behavior the language leaves undefined ([RFC 0051](/rfcs/0051-nothing-undefined/); [the list](/reference/undefined/) item by item). What no rule rejects has one result or stops with a report, in both builds:

- signed arithmetic wraps, a shift's count is taken modulo the width, and a conversion from floating point saturates; dividing by zero, or the least value by -1, stops (`division-by-zero`, `division-overflow`);
- `std::unreachable()` reached stops (`unreachable`), and so does the end of a function that must return a value (`missing-return`): a `switch` over an `enum class` given another value no longer jumps through a table unchecked;
- every local starts as zeroes, a loop may run forever, and a full stack stops (`stack-exhaustion`);
- nothing the code asserts is believed: `[[assume]]`, `__builtin_assume`, `[[gnu::pure]]` and their kin are errors ([`assumed`](/reference/rules/#assumed)), whose certain fix `--weavec-fix` applies, and `-ffast-math` and the other flags that assume are refused;
- bytes do not become a `bool` or a pointer ([`pointer-bytes`](/reference/rules/#pointer-bytes)), a handler the system runs between any two instructions is registered by code that is not safe ([`signal-handler`](/reference/rules/#signal-handler)), and `-Wreturn-type`, `-Winvalid-noreturn`, `-Wformat`, `-Wformat-security` and `-Wunsequenced` are errors;
- the units of a program agree: two files that each define a `struct Entry` of their own, or a header compiled with two values of a macro, do not link ([`definition-mismatch`](/reference/rules/#definition-mismatch)). Put a type one file uses in an unnamed namespace.

## Building blocks

Each of these compiles under `-fweavec-safe` and is checked with the documentation.

### A value class

Members that are objects own; getters return borrows of what the caller lent; anything stored is taken by value and moved in.

```cpp accepted
struct Item { int id = 0; std::string name; std::vector<std::string> tags; };

class Inventory {
public:
  void add(Item item) { items_.push_back(std::move(item)); }
  const Item *find(int id) const {
    for (const Item &item : items_)
      if (item.id == id)
        return &item;
    return nullptr;
  }
  bool rename(int id, std::string name) {
    for (Item &item : items_)
      if (item.id == id) {
        item.name = std::move(name);   // changes an element, not the vector
        return true;
      }
    return false;
  }
  bool remove(int id) {
    return std::erase_if(items_, [id](const Item &item) { return item.id == id; }) != 0;
  }
private:
  std::vector<Item> items_;
};
```

### A tree with a parent pointer

Children are owned by `std::unique_ptr`. The parent pointer is a reference: read through it, never change through it. Put the child in its owner first and take the pointer from there; a pointer taken from a `unique_ptr` ends when the `unique_ptr` is moved.

```cpp accepted
struct Node {
  std::string label;
  std::vector<std::unique_ptr<Node>> children;
  const Node *parent = nullptr;

  Node *add(std::string text) WEAVEC_ESCAPES { // keeps `this` in the child
    auto child = std::make_unique<Node>();
    child->label = std::move(text);
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();        // taken from the owner
  }
  int depth() const { return parent != nullptr ? parent->depth() + 1 : 0; }
  std::size_t count() const {
    std::size_t total = 1;
    for (const auto &child : children)
      total += child->count();
    return total;
  }
};
```

### Tokens that are views, and tokens that own

A token with a `std::string_view` is a view of the source: fast, and tied to the source's lifetime by the compiler. A function may return such tokens only for a source its caller passed in. When tokens must outlive the text, they own their strings.

```cpp accepted
struct Token { std::string_view text; int line = 0; };

std::vector<Token> lex(std::string_view source) {
  std::vector<Token> tokens;
  std::size_t start = 0;
  int line = 1;
  for (std::size_t i = 0; i <= source.size(); ++i) {
    if (i < source.size() && source[i] != ' ' && source[i] != '\n')
      continue;
    if (i > start)
      tokens.push_back(Token{source.substr(start, i - start), line});
    if (i < source.size() && source[i] == '\n')
      ++line;
    start = i + 1;
  }
  return tokens;                         // views of what the caller keeps
}

struct OwnedToken { std::string text; int line = 0; };

std::vector<OwnedToken> lex_file(const std::string &contents) {
  std::vector<OwnedToken> owned;
  for (const Token &token : lex(contents))
    owned.push_back(OwnedToken{std::string(token.text), token.line});
  return owned;
}
```

### A hierarchy walked by kind

A class hierarchy whose base says which class an object is, walked with a `switch` and a `static_cast`, is accepted as it is: the cast is checked when it runs ([RFC 0054](/rfcs/0054-one-of-several-types/)), for a class with virtual functions by one comparison of the type information its vtable names. A wrong one stops with `bad-cast`. `dynamic_cast` stays accepted, and costs more.

```cpp accepted
enum class Kind { Literal, Binary };
struct Expr { Kind kind; explicit Expr(Kind k) : kind(k) {} virtual ~Expr() = default; };
struct Literal final : Expr { double value = 0; Literal() : Expr(Kind::Literal) {} };
struct Binary final : Expr {
  char op = '+';
  std::unique_ptr<Expr> left, right;
  Binary() : Expr(Kind::Binary) {}
};

double evaluate(const Expr &expr) {
  switch (expr.kind) {
  case Kind::Literal:
    return static_cast<const Literal &>(expr).value;
  case Kind::Binary: {
    const auto &binary = static_cast<const Binary &>(expr);
    const double a = evaluate(*binary.left), b = evaluate(*binary.right);
    return binary.op == '+' ? a + b : a * b;
  }
  }
  return 0;
}
```

A `static_cast` from `void *` to a class (a C library's callback context) is checked the same way, and so is a union with a tag, as in Safe C (`WEAVEC_TAGGED_BY`, `WEAVEC_WHEN`; members without destructors).

### A variant and its visitor

`std::variant` replaces unions; a reference into the alternative ends when the variant is assigned.

```cpp accepted
struct Number { double value = 0; };
struct Text { std::string value; };
struct List;
using Value = std::variant<Number, Text, std::unique_ptr<List>>;
struct List { std::vector<Value> items; };

std::size_t size_of(const Value &value) {
  struct Visitor {
    std::size_t operator()(const Number &) const { return 1; }
    std::size_t operator()(const Text &text) const { return text.value.size(); }
    std::size_t operator()(const std::unique_ptr<List> &list) const {
      std::size_t total = 0;
      for (const Value &item : list->items)
        total += size_of(item);
      return total;
    }
  };
  return std::visit(Visitor{}, value);
}
```

### An index instead of a pointer

Where objects refer to each other and the set changes, keep indices (or keys) and look the object up when it is needed. A handle that is a number cannot dangle.

```cpp accepted
struct Employee { std::string name; std::optional<std::size_t> manager; };

class Directory {
public:
  std::size_t add(std::string name, std::optional<std::size_t> manager) {
    people_.push_back(Employee{std::move(name), manager});
    return people_.size() - 1;
  }
  std::string chain(std::size_t who) const {
    std::string out = people_.at(who).name;
    for (auto next = people_.at(who).manager; next; next = people_.at(*next).manager)
      out += " < " + people_.at(*next).name;
    return out;
  }
private:
  std::vector<Employee> people_;
};
```

### An LRU cache

The map owns the entries. The order is kept by links that hold pointers to each other, which are references, and that own nothing: a function may be handed a pointer to a link and change it, where it could not be handed a pointer kept in memory to an object that owns a string (`owner-path`).

```cpp accepted
struct Link {                              // owns nothing
  int key = 0;
  Link *newer = nullptr;
  Link *older = nullptr;
};
struct Entry {
  std::string value;
  Link link;
};

struct Order {
  Link *newest = nullptr;
  Link *oldest = nullptr;
  void unlink(Link *link) {
    if (link->newer != nullptr) link->newer->older = link->older; else newest = link->older;
    if (link->older != nullptr) link->older->newer = link->newer; else oldest = link->newer;
    link->newer = nullptr;
    link->older = nullptr;
  }
  void push(Link *WEAVEC_ESCAPES link) { // keeps the link
    link->older = newest;
    if (newest != nullptr) newest->newer = link; else oldest = link;
    newest = link;
  }
};

class Cache {
public:
  explicit Cache(std::size_t capacity) : capacity_(capacity) {}
  const std::string *get(int key) {
    const auto found = entries_.find(key);
    if (found == entries_.end())
      return nullptr;
    Entry &entry = *found->second;
    order_.unlink(&entry.link);
    order_.push(&entry.link);
    return &entry.value;
  }
  void put(int key, std::string value) {
    if (const auto found = entries_.find(key); found != entries_.end()) {
      Entry &entry = *found->second;
      entry.value = std::move(value);
      order_.unlink(&entry.link);
      order_.push(&entry.link);
      return;
    }
    if (entries_.size() == capacity_ && order_.oldest != nullptr) {
      const int evicted = order_.oldest->key;
      order_.unlink(order_.oldest);
      entries_.erase(evicted);
    }
    auto entry = std::make_unique<Entry>();
    entry->value = std::move(value);
    entry->link.key = key;
    const auto placed = entries_.emplace(key, std::move(entry)).first;
    order_.push(&placed->second->link);    // taken from the owner
  }
private:
  std::size_t capacity_;
  std::unordered_map<int, std::unique_ptr<Entry>> entries_;
  Order order_;
};
```

### Callbacks

A callable that captures by reference is a view. Pass it down freely, as a template parameter or as a `std::function` for the call, but not beside an object it changes (`call-alias`). One that is kept (in a member, an element, a `static`, by a parameter that keeps it), of any type, a binder or a functor as well as a lambda, captures by value: copies, moved owners, or a `std::shared_ptr` or `std::weak_ptr` to what it needs; it captures no reference, no `this` and no pointer. A callback that needs its object takes it as an argument (`std::function<void(Panel &)>`), or, for a class shared by `std::shared_ptr`, captures `[weak = weak_from_this()]` and locks it. A callable that a local of the same function keeps (`Timer timer([&count] { … })`) is one the function sees, and a call on the local may run it. A function that keeps a callable takes it by value (`std::function<void()> handler`), and one that only calls it takes it by reference to `const` or as a template parameter: the compiler rejects a caller that passes a lambda capturing a local by reference where it would be kept past the local, and rejects a function that keeps a callable it took by reference. A callable returned as a `std::function` owns what it captured. (`WEAVEC_ESCAPES`, written after a parameter's type, says "kept" where the type does not.)

```cpp accepted
template <typename F> void each_even(const std::vector<int> &values, F visit) {
  for (int value : values)
    if (value % 2 == 0)
      visit(value);
}

int sum_changed(const std::vector<int> &values, const std::function<int(int)> &change) {
  int total = 0;
  for (int value : values)
    total += change(value);
  return total;
}

int use(const std::vector<int> &values) {
  int evens = 0;
  each_even(values, [&](int) { ++evens; });               // used during the call
  const int scale = 3;
  return evens + sum_changed(values, [&](int value) { return value * scale; });
}

class Button {
public:
  void on_click(std::function<void()> handler) { handlers_.push_back(std::move(handler)); }
  void click() const {
    for (const auto &handler : handlers_)
      handler();
  }
private:
  std::vector<std::function<void()>> handlers_;
};

void wire(Button &button, std::shared_ptr<int> clicks) {
  button.on_click([clicks] { ++*clicks; });                // kept: captures by value
}
```

### Sharing

`std::shared_ptr<const T>` shares an object nobody changes, and the rules are complete for it. An object changed while it is shared, through a `std::shared_ptr<T>` or a raw pointer member, is of a class declared `WEAVEC_ALIASED`, a class that locks its members, or a library type with no storage that changes type in place; a fresh `std::make_shared` local that is only dereferenced and moved is the function's own until it is published. An aliased class holds no `std::variant`, `std::any` or `std::function`, and the rules do not follow the other holders: a borrow another function holds into the same object is checked when the program runs.

```cpp accepted
struct Config { std::string name; std::vector<std::string> paths; };

class Service {
public:
  explicit Service(std::shared_ptr<const Config> config) : config_(std::move(config)) {}
  const std::string &name() const { return config_->name; }
private:
  std::shared_ptr<const Config> config_;
};

std::vector<Service> start(std::size_t count, Config config) {
  const auto shared = std::make_shared<const Config>(std::move(config));
  std::vector<Service> services;
  for (std::size_t i = 0; i < count; ++i)
    services.emplace_back(shared);
  return services;
}
```

### What needs raw memory

A class that manages memory or a C library's handle itself is the implementation of an owner. Keep it small and mark it `WEAVEC_GUARDED`; safe code uses it as it uses `std::vector`. The [audit](/reference/cli/) lists every such class and function, with the pointer members, shared members and type-erased members the rules do not follow.

```cpp accepted
class WEAVEC_GUARDED File {
public:
  explicit File(const std::string &path) : handle_(std::fopen(path.c_str(), "r")) {}
  ~File() { if (handle_ != nullptr) std::fclose(handle_); }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  std::optional<std::string> line() {
    char buffer[256];
    if (handle_ == nullptr || std::fgets(buffer, sizeof buffer, handle_) == nullptr)
      return std::nullopt;
    return std::string(buffer);
  }
private:
  std::FILE *handle_;
};

std::size_t count_lines(const std::string &path) {
  File file(path);
  std::size_t lines = 0;
  while (file.line())
    ++lines;
  return lines;
}
```

### A signal handler

A handler runs between any two instructions of the program, so safe code does not register one. A small guarded function does, and the handler only sets an atomic that safe code reads:

```cpp accepted
std::atomic<bool> stopping{false};

void onStop(int) { stopping.store(true); }

WEAVEC_GUARDED void installStop() {
  struct sigaction action {};
  action.sa_handler = onStop;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

int serve() {
  installStop();
  std::signal(SIGPIPE, SIG_IGN);
  int handled = 0;
  while (!stopping.load() && handled < 3)
    ++handled;
  return handled;
}
```

## Threads

Safe C++ has no data races, and the rules alone say so: nothing at run time checks them, in either build ([RFC 0046](/rfcs/0046-thread-safety/)). Three facts about a type decide what may cross into a thread. A **sendable** type can be handed to another thread: it owns what it holds, and its pointers point to what is shareable. A **shareable** type can be used by two threads at once: an atomic, a condition variable, a mutex, or a class whose members are each shareable, `const`, or guarded by the class's one lock. A **frozen** type is never written through `const`. Then:

- what a `std::jthread`, a `std::thread` or `std::async` is given by value is sendable (`thread-send`);
- what it reaches by reference is shareable, a `const` local of frozen type, a `std::shared_ptr<const T>`'s object, or a local the function lends it and keeps off until the thread ends (`thread-share`, `thread-lent`);
- in a class with a `std::mutex` member, the other members are used only under a guard on it, and a borrow of one ends with the guard (`lock-held`);
- a global is shareable, `const`, or `thread_local`, in every program.

The forms an agent ports first, each one compiled as safe C++:

### Tasks that own their input

A task given what it needs by value, returning its result through a future, shares nothing.

```cpp accepted
std::size_t longest(std::vector<std::string> lines) {
  std::size_t best = 0;
  for (const std::string &line : lines)
    best = std::max(best, line.size());
  return best;
}

std::size_t longest_of_all(std::vector<std::vector<std::string>> batches) {
  std::vector<std::future<std::size_t>> results;
  for (std::vector<std::string> &batch : batches)
    results.push_back(std::async(std::launch::async, longest, std::move(batch)));
  std::size_t best = 0;
  for (std::future<std::size_t> &result : results)
    best = std::max(best, result.get());
  return best;
}
```

### Readers of a `const` local, and a local lent to one worker

A `const` local of frozen type is immutable: any number of threads read it. A local that is not `const` is lent to the thread that borrows it, and the function keeps off it until the thread object ends. Threads that each write their own element of one vector, at the index of the loop that starts them, are lent that element only.

```cpp accepted
long sum_in_parallel(const std::vector<int> &input) {
  const std::vector<int> data = input;      // an immutable copy
  std::vector<long> parts(4);
  {
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i)
      workers.emplace_back([&data, &parts, i] {
        for (std::size_t k = static_cast<std::size_t>(i); k < data.size(); k += 4)
          parts[static_cast<std::size_t>(i)] += data[k];
      });
  }                                         // every worker is joined here
  long total = 0;
  for (const long part : parts)
    total += part;
  return total;
}

std::size_t count_words(const std::string &text) {
  std::size_t words = 0;
  {
    std::jthread counter([&words, copy = text] {
      bool in = false;
      for (const char c : copy) {
        if (c != ' ' && !in)
          ++words;
        in = c != ' ';
      }
    });
  }                                         // 'words' is the function's again
  return words;
}
```

### An atomic counter

```cpp accepted
int count_in_parallel(int per_thread) {
  std::atomic<int> count{0};
  {
    std::jthread a([&count, per_thread] { for (int i = 0; i < per_thread; ++i) ++count; });
    std::jthread b([&count, per_thread] { for (int i = 0; i < per_thread; ++i) ++count; });
  }
  return count.load();
}
```

### A queue: a class that locks its members

The class's one `std::mutex` guards its other members, so the class is shareable and threads may hold it by reference. A value leaves the lock by copy or by move; a reference into `items_` would outlive the guard and is `escape`.

```cpp accepted
class Queue {
public:
  void push(std::string item) {
    {
      const std::lock_guard lock(mu_);
      items_.push_back(std::move(item));
    }
    ready_.notify_one();
  }
  std::optional<std::string> pop() {
    std::unique_lock lock(mu_);
    ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
    if (items_.empty())
      return std::nullopt;
    std::string item = std::move(items_.front());
    items_.erase(items_.begin());
    return item;
  }
  void close() {
    {
      const std::lock_guard lock(mu_);
      closed_ = true;
    }
    ready_.notify_all();
  }
private:
  std::mutex mu_;
  std::condition_variable ready_;
  std::vector<std::string> items_;
  bool closed_ = false;
};

std::size_t consume(const std::vector<std::string> &lines) {
  Queue queue;
  std::size_t total = 0;
  {
    std::jthread worker([&queue, &total] {
      while (std::optional<std::string> line = queue.pop())
        total += line->size();
    });
    for (const std::string &line : lines)
      queue.push(line);                     // 'queue' is shareable: not lent
    queue.close();
  }
  return total;
}
```

### A class that owns its worker thread

The thread uses `this`, so the class is shareable, cannot be copied or moved away from its thread, and declares the thread last, so that it is joined before the other members are destroyed (a class with virtual functions is `final` too).

```cpp accepted
class Ticker {
public:
  Ticker() : thread_([this](std::stop_token stop) { run(stop); }) {}
  Ticker(const Ticker &) = delete;
  Ticker &operator=(const Ticker &) = delete;
  int ticks() const { return ticks_.load(); }
private:
  void run(const std::stop_token &stop) {
    while (!stop.stop_requested())
      ++ticks_;
  }
  std::atomic<int> ticks_{0};
  std::jthread thread_;                     // last
};
```

### A pool that takes tasks

A `std::function` does not say what it captured. A parameter that keeps one to run on another thread says `WEAVEC_SENDS`, after its type; callers then pass a callable that captures by value only what may be sent.

```cpp accepted
class Pool {
public:
  explicit Pool(int threads) {
    for (int i = 0; i < threads; ++i)
      workers_.emplace_back([this] { run(); });
  }
  Pool(const Pool &) = delete;
  Pool &operator=(const Pool &) = delete;
  ~Pool() {
    {
      const std::lock_guard lock(mu_);
      stop_ = true;
    }
    ready_.notify_all();
  }
  void submit(std::function<void()> WEAVEC_SENDS task) {
    {
      const std::lock_guard lock(mu_);
      tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
  }
private:
  void run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mu_);
        ready_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (tasks_.empty())
          return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }
  std::mutex mu_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  bool stop_ = false;
  std::vector<std::jthread> workers_;
};

void log_all(Pool &pool, std::vector<std::string> lines) {
  auto total = std::make_shared<std::atomic<std::size_t>>(0);
  for (std::string &line : lines)
    pool.submit([total, line = std::move(line)] { *total += line.size(); });
}
```

### A snapshot, and `Locked<T>`

A `std::shared_ptr<const T>` made by `std::make_shared` shares an object no holder can change; the pointer itself is replaced under a lock. `Locked<T>`, ten lines of safe C++, is Rust's `Mutex<T>`: the value is reached only through `with`, under the lock, and a callable that returns a reference into it is `escape` where `with` is instantiated.

```cpp accepted
struct Routes { std::map<std::string, int> backends; };

class Router {
public:
  std::shared_ptr<const Routes> snapshot() const {
    const std::lock_guard lock(mu_);
    return routes_;
  }
  void add(const std::string &prefix, int backend) {
    auto next = std::make_shared<Routes>(*snapshot());
    next->backends[prefix] = backend;
    const std::lock_guard lock(mu_);
    routes_ = std::move(next);             // no other handle can change it
  }
private:
  mutable std::mutex mu_;
  std::shared_ptr<const Routes> routes_ = std::make_shared<const Routes>();
};

template <class T> class Locked {
public:
  explicit Locked(T value) : value_(std::move(value)) {}
  template <class F> auto with(F f) {
    const std::lock_guard lock(mu_);
    return f(value_);
  }
private:
  mutable std::mutex mu_;
  T value_;
};

std::size_t record(Locked<std::vector<std::string>> &log, std::string line) {
  return log.with([&line](std::vector<std::string> &lines) {
    lines.push_back(std::move(line));
    return lines.size();
  });
}
```

What the rules reject that is correct: a mutex beside the data it guards in locals or globals (put both in a class); several threads writing one container other than slot by slot; publication by release and acquire; a class with two locks; locking by hand; a `const` member function writing through a `std::unique_ptr`. Each has a rewrite in its error's note, and `weavec-c++ --weavec-explain <rule>` shows it.

## Coming from Rust

| Rust                                                                   | Safe C++                                                                                                                                                                                                 |
| ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Box<T>`, `Vec<T>`, `String`, `Option<T>`                              | `std::unique_ptr<T>`, `std::vector<T>`, `std::string`, `std::optional<T>`                                                                                                                                |
| a move, and "use of moved value"                                       | `std::move`, and `owner-moved`; `std::exchange(x, {})` is `mem::take`                                                                                                                                    |
| `&T`, `&mut T` in a local or a parameter                               | `const T &`, `T &`, pointers, iterators: borrows, ended by a non-`const` use of what they point into                                                                                                     |
| "cannot borrow as mutable because…"                                    | `stale-borrow` at the later use; two `int &` into one vector are fine, since changing an `int` ends nothing                                                                                              |
| `&str`, `&[T]`, a struct with a lifetime                               | `std::string_view`, `std::span<T>` or a pointer with a bound (`WEAVEC_COUNTED_BY`), a class with one as a member: views; there are no lifetime parameters, and a view's borrows are worked out per local |
| returning or storing a borrow of a local                               | `escape`                                                                                                                                                                                                 |
| `&mut self` twice (`self.f(&mut self.x)`)                              | `call-alias`                                                                                                                                                                                             |
| a back pointer: `Weak<T>`, an index                                    | a raw pointer member (read only, checked at run time), or an index                                                                                                                                       |
| `Rc<T>`                                                                | `std::shared_ptr<const T>`                                                                                                                                                                               |
| `Rc<RefCell<T>>`                                                       | `std::shared_ptr<T>` to a `WEAVEC_ALIASED` class: conflicting borrows are found at run time by the guards, not by a count; no enum (`std::variant`) inside                                               |
| `enum` and `match`                                                     | a `std::variant` and `std::visit`; or a class hierarchy walked by a kind and a `static_cast`, checked when it runs (`bad-cast`); or a union with `WEAVEC_TAGGED_BY` (`union-member`)                     |
| `Any::downcast_ref`, a trait object downcast                           | a `static_cast` from `void *` or to a derived class, checked when it runs                                                                                                                                |
| a slice index or `unwrap()` that panics                                | a hardened library check (`library-assertion`) or a `null-dereference` report                                                                                                                            |
| `unsafe`                                                               | `WEAVEC_GUARDED` (still guarded at run time), `WEAVEC_UNSAFE` (not)                                                                                                                                      |
| integer overflow (a panic in a debug build, wrapping in a release one) | wrapping in both builds; a division by zero stops (`division-by-zero`)                                                                                                                                   |
| `unreachable!()`, a `match` that must be exhaustive                    | `std::unreachable()` and the end of a function that must return a value stop with a report                                                                                                               |
| `bool` is 0 or 1; `transmute` is `unsafe`                              | bytes do not become a `bool` or a pointer (`pointer-bytes`); `std::bit_cast` and byte views of objects without either                                                                                    |
| a crate's one definition of each name                                  | the link compares what safe units record of the names they share (`definition-mismatch`)                                                                                                                 |
| `Send`                                                                 | a sendable type; `thread-send`                                                                                                                                                                           |
| `Sync`                                                                 | a shareable type, or a `const` object of a frozen type; `thread-share`                                                                                                                                   |
| `thread::scope` and a borrowed local                                   | a `std::jthread` local and a lent local; `thread-lent`                                                                                                                                                   |
| `thread::spawn` and `'static`                                          | a thread object that borrows nothing, which may be detached                                                                                                                                              |
| `Mutex<T>` and its guard                                               | a class with a `std::mutex` member, and `std::lock_guard`; `lock-held`                                                                                                                                   |
| `RwLock<T>`                                                            | `std::shared_mutex` and `std::shared_lock`                                                                                                                                                               |
| `Arc<T>`, `Arc<Mutex<T>>`                                              | `std::shared_ptr<const T>`, a `std::shared_ptr` to a locked class                                                                                                                                        |
| `static`, `thread_local!`                                              | a shareable or `const` global, `thread_local`                                                                                                                                                            |
| `Box<dyn FnOnce() + Send>`                                             | a `std::function<void()> WEAVEC_SENDS` parameter                                                                                                                                                         |
| `chunks_mut` over a slice                                              | threads that write `v[i]` at their own loop index                                                                                                                                                        |

## What the rules do not see

Each of these stops when it runs in the checked build (`-fweavec-safe-checks=full`); in the fast build an index or iterator out of range still stops, and a use after free reaches an object of the same type or is reported while the block is not yet reused:

- a dangling raw pointer member or element (a reference);
- an object of a `WEAVEC_ALIASED` class that another holder changed under a borrow;
- a stored callable that changes the container it is being called from, through a `std::shared_ptr` it captured;
- an element moved out of a container (`std::move(v[i])`) and used again;
- the address of a pointer parameter itself (`return &p;`), and views kept in a C array (`std::string_view parts[4]`; a `std::array` of them is followed);
- a container's iterators passed to a standard algorithm the standard forbids them for (`v.assign(v.begin() + 1, v.end())`);
- an index or an iterator out of range, a null `std::unique_ptr`, an empty `std::optional`;
- races that are not data races (a check and an act in two locked calls), deadlock and lock order, and what code that is not safe does with threads or with what safe code shares.

The [guarantees](/reference/guarantees/) page states exactly what holds, (X1) to (X6), and what it assumes of code that is not safe.
