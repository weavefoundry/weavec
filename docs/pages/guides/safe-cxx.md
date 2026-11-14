---
title: Write safe C++
description: Compile a C++ unit with weavec-c++ -fweavec-safe so that a use after std::move, a reference kept across push_back, a string_view of a temporary and a view that outlives its buffer stop the compile, with errors you or an agent can act on.
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

## Owners, borrows, views and references

- An **owner** is an object: a local, a member, an element of a container, what a `std::unique_ptr` or a `std::optional` holds. Destructors release, so safe C++ writes no `new` and no `delete` (`raw-owner`).
- A **borrow** is a reference, a pointer, an iterator, a `std::string_view`, a `std::span` or a lambda that captures by reference, kept in a local or passed down a call. It ends when what it points into is destroyed, moved, assigned or changed (`stale-borrow`).
- A **view** is an object whose type holds a borrow: a struct with a `std::string_view` or a reference member, a `std::vector<std::string_view>`, a `std::function` made from a capturing lambda. It is not returned or stored past what it points into (`escape`).
- A **reference** is a raw pointer kept in a member or an element. The rules do not follow it, and the guards check it when it is used. What an object owns is not changed through one (`owner-path`).

A class chooses between the last two when it declares a member: `Lexer &lexer_` (or a `std::string_view`) is checked when the program compiles and ties the object to a scope; `Lexer *lexer_` may live anywhere and is checked when it runs.

### What ends a borrow

A borrow into an owner ends when the owner is destroyed, moved or assigned, when a member function that is not `const` is called on it, and when it is passed to a function through a reference or pointer that is not `const`. Two things keep this from being as strict as it sounds:

- A member function that has a `const` overload with the same parameters only gives access, and ends nothing: `operator[]`, `at`, `front`, `data`, `begin`, `end`, `find`. (`operator[]` of a `std::map` inserts, has no `const` overload, and ends borrows; use `at` or `find`.)
- Changing an _element_ ends borrows into that element's own data, not borrows of the container's elements: a loop that edits each item is fine, a loop that pushes to the vector it iterates is not.

As in Rust, the fix is one of: take the borrow again after the change, keep an index, copy the value out first, or declare the callee's parameter `const`.

### Fast and checked builds

A safe unit is built one of two ways, as Rust builds a debug and a release binary:

| `-fweavec-safe-checks=` | What it is for                                 | At run time                                                                                                                                                                                                                                                                           |
| ----------------------- | ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `bounds` (the default)  | the build you ship and benchmark               | allocations carry their type and a block safe code releases is reused at once by its own type; no guard on an access through a reference at a constant offset, none in the standard library's functions whose preconditions its hardening checks; indices and iterators still checked |
| `full`                  | the build you test, and an agent iterates with | exactly a guarded unit: every guard, the quarantine and the sweep, allocation stacks, container annotations; every bug the rules miss stops with a full report                                                                                                                        |

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

  Node *add(std::string text) {
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

### A variant and its visitor

`std::variant` replaces unions and downcasts; a reference into the alternative ends when the variant is assigned.

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
  void push(Link *link) {
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

A callable that captures by reference is a view. Pass it down freely, as a template parameter or as a `std::function` for the call. One that is kept captures by value: copies, moved owners, or a `std::shared_ptr` to what it needs. A function that keeps a callable takes it by value (`std::function<void()> handler`), and one that only calls it takes it by reference to `const` or as a template parameter: the compiler rejects a caller that passes a lambda capturing a local by reference where it would be kept past the local, and rejects a function that keeps a callable it took by reference. A callable returned as a `std::function` owns what it captured. (`WEAVEC_ESCAPES`, written after a parameter's type, says "kept" where the type does not.)

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

`std::shared_ptr<const T>` shares an object nobody changes, and the rules are complete for it. `std::shared_ptr<T>` shares an object that changes: safe C++ allows the change and does not follow the other holders, so a borrow another function holds into the same object is checked only when the program runs.

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

## Coming from Rust

| Rust                                      | Safe C++                                                                                                                                                 |
| ----------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Box<T>`, `Vec<T>`, `String`, `Option<T>` | `std::unique_ptr<T>`, `std::vector<T>`, `std::string`, `std::optional<T>`                                                                                |
| a move, and "use of moved value"          | `std::move`, and `owner-moved`; `std::exchange(x, {})` is `mem::take`                                                                                    |
| `&T`, `&mut T` in a local or a parameter  | `const T &`, `T &`, pointers, iterators: borrows, ended by a non-`const` use of what they point into                                                     |
| "cannot borrow as mutable because…"       | `stale-borrow` at the later use; two `int &` into one vector are fine, since changing an `int` ends nothing                                              |
| `&str`, `&[T]`, a struct with a lifetime  | `std::string_view`, `std::span<T>`, a class with one as a member: views; there are no lifetime parameters, and a view's borrows are worked out per local |
| returning or storing a borrow of a local  | `escape`                                                                                                                                                 |
| `&mut self` twice (`self.f(&mut self.x)`) | `call-alias`                                                                                                                                             |
| a back pointer: `Weak<T>`, an index       | a raw pointer member (read only, checked at run time), or an index                                                                                       |
| `Rc<T>`                                   | `std::shared_ptr<const T>`                                                                                                                               |
| `Rc<RefCell<T>>`                          | `std::shared_ptr<T>`: allowed; conflicting borrows are found at run time by the guards, not by a count                                                   |
| a slice index or `unwrap()` that panics   | a hardened library check (`library-assertion`) or a `null-dereference` report                                                                            |
| `unsafe`                                  | `WEAVEC_GUARDED` (still guarded at run time), `WEAVEC_UNSAFE` (not)                                                                                      |
| `Send`, `Sync`                            | nothing yet: data races are not checked                                                                                                                  |

## What the rules do not see

Each of these stops when it runs in the checked build (`-fweavec-safe-checks=full`); in the fast build an index or iterator out of range still stops, and a use after free reaches an object of the same type or is reported while the block is not yet reused:

- a dangling raw pointer member or element (a reference);
- an object shared by `std::shared_ptr<T>` that another holder changed under a borrow;
- a callable that captured `this`, or an object a parameter refers to, and is kept by an object that outlives it; and a stored callable that changes the container it is being called from;
- an element moved out of a container (`std::move(v[i])`) and used again;
- the address of a pointer parameter itself (`return &p;`), and views kept in a C array (`std::string_view parts[4]`; a `std::array` of them is followed);
- a container's iterators passed to a standard algorithm the standard forbids them for (`v.assign(v.begin() + 1, v.end())`);
- an index or an iterator out of range, a null `std::unique_ptr`, an empty `std::optional`;
- globals that are not `const`, and data races.

The [guarantees](/reference/guarantees/) page states exactly what holds, (X1) to (X6), and what it assumes of code that is not safe.
