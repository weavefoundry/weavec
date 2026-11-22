# The rules of Safe C and Safe C++

A safe function (`WEAVEC_SAFE`, or every function of a unit compiled with `-fweavec-safe`) is checked against these rules when it compiles. Each error ends with the rule's identifier, `[weavec-safe:<rule>]`, and `weavec-cc --weavec-explain <rule>` (or `weavec-c++`) prints the rule's section of this page; `--weavec-explain all` prints all of it. Every rule is decided inside one function, from the declarations it can see. A rule rejects a construct and says what to write instead: none tries to prove that code it does not accept is correct. What cannot be written in a safe function goes in a small `WEAVEC_GUARDED` helper, which `weavec-cc --weavec-audit` lists.

A rule that both languages have says first what it means in C, then, under _In C++_, what it means there. [`safe-cxx`](#safe-cxx) is the page to read before writing safe C++: the kinds of pointer its rules are stated over.

The C examples below include `<weavec.h>`, `<signal.h>`, `<stdbool.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>` and `<string.h>`; the C++ ones `<weavec.h>`, `<bit>`, `<csignal>` and the standard library's containers, strings, smart pointers and algorithms, as C++20. The design is in [RFC 0041](/rfcs/0041-safe-c/) (bounds), [RFC 0042](/rfcs/0042-typed-memory/) (typed memory), [RFC 0043](/rfcs/0043-ownership/) (ownership), [RFC 0044](/rfcs/0044-safe-cxx/) (C++), [RFC 0046](/rfcs/0046-thread-safety/) (threads), [RFC 0047](/rfcs/0047-conversion-in-place/) (bounds and aliased classes in C++, converting existing code) and [RFC 0048](/rfcs/0048-safe-code-without-the-shadow/) (kept parameters, ends, what a fast build believes). The thread examples include `<atomic>`, `<condition_variable>`, `<future>`, `<mutex>`, `<shared_mutex>` and `<thread>` too.

## safe-cxx

Not a rule: what the rules of safe C++ are about. `weavec-c++ -fweavec-safe` checks every function the unit defines in its own files (not the standard library's or anything else behind `-isystem`), each lambda, and each instantiation of its templates. The checked build (`-fweavec-safe-checks=full`, the default of a compile without optimization) compiles a safe C++ function exactly as without the flag: every guard and every check of the hardened standard library stays, so what the rules do not see still stops when it runs. The fast build, the default with optimization, checks the function against what it holds instead: a pointer's type, a bound, a string's terminator (RFC 0048).

Every pointer, reference and object in a safe function is one of four kinds:

- An **owner** is an object: a local, a member, an element, and what a `std::vector`, a `std::string`, a `std::unique_ptr` or a `std::optional` holds. Destructors release, so a safe function writes no `new` and no `delete` ([`raw-owner`](#raw-owner)).
- A **borrow** is a reference, a pointer, an iterator, a `std::string_view`, a `std::span` or a lambda that captures by reference, kept in a local or passed down a call. It ends when what it points into is destroyed, moved, assigned, or changed by anything that is not `const`; using it afterwards is [`stale-borrow`](#stale-borrow). A member function that has a `const` overload with the same parameters (`operator[]`, `at`, `begin`, `find`) only gives access and ends nothing.
- A **view** is an object whose type holds a borrow: a struct with a `std::string_view` or a reference member, a `std::vector<std::string_view>`, a `std::function` made from a lambda that captures by reference. The rules follow it like a borrow: it is not used after what it points into ended, and it is not returned or stored where it would outlive that ([`escape`](#escape)).
- A **reference** is a raw pointer kept in a member or in a container's element. The rules do not follow it: the checked build's guards check it when it is used, and the fast build believes it points to an object of its type, live or released. What an object owns is not changed through one, nor through a `std::shared_ptr`, unless its class is declared `WEAVEC_ALIASED` ([`owner-path`](#owner-path)). A function that keeps the address of what a parameter refers to, or of `this`, says so with `WEAVEC_ESCAPES` ([`escape`](#escape)); a local whose address is kept lives in typed memory for its scope.

A raw pointer points to one object unless its declaration, an array or the library says how many: Safe C's bounds (`WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(e)`, `WEAVEC_NULL_TERMINATED` and the rest) mean the same in C++, on parameters, results, members and globals; an array keeps its length when it becomes a pointer; a container's `data()` has its `size()` elements; `main`'s `argv` has `argc`. Such a pointer is indexed and stepped, and each access is compared with its bound when it runs ([`index-single`](#index-single), [`call-args`](#call-args), [`counted-pair`](#counted-pair), [`field-address`](#field-address)). A `std::span` is the same thing as a type; existing code keeps its pointers and declares their bounds.

The other flow rules are [`owner-moved`](#owner-moved) (an object used after `std::move`) and [`call-alias`](#call-alias). The rules of single expressions are [`index-single`](#index-single), [`pointer-cast`](#pointer-cast), [`pointer-bytes`](#pointer-bytes), [`union-pointer`](#union-pointer), [`int-to-pointer`](#int-to-pointer), [`variadic`](#variadic), [`no-asm`](#no-asm), [`assumed`](#assumed), [`signal-handler`](#signal-handler), [`unsafe-block`](#unsafe-block), [`bad-annotation`](#bad-annotation) and [`not-modeled`](#not-modeled). Rules without an _In C++_ part are C's. A safe function has no behavior the language leaves undefined ([RFC 0051](/rfcs/0051-nothing-undefined/)): what no rule rejects is defined or stops when it runs ([`defined`](#defined)), safe units that disagree do not link ([`definition-mismatch`](#definition-mismatch)), and `--weavec-explain undefined` lists every item of the two languages' lists and what disposes of it.

Threads add three facts about a type, each read from its structure ([RFC 0046](/rfcs/0046-thread-safety/)). A type is **sendable** when a value of it can be handed to another thread: what it owns is its own, and every pointer it holds points to something shareable (`std::vector<std::string>`, `std::unique_ptr<Job>`, a `std::future<int>`; not a `std::string_view`, a `std::shared_ptr<State>` of a plain struct, or a `std::function`, whose type does not say what it captured). A type is **shareable** when two threads can use one object of it at once: an atomic, a condition variable, a `std::mutex`, or a class each of whose members is shareable, `const` and frozen, a thread member, or guarded by the class's one lock member (a **locked class**). A type is **frozen** when an object of it reached only through `const` is never written: no `mutable` member that is not shareable, and no pointer to what is not shareable. Then: what a thread is given by value is sendable ([`thread-send`](#thread-send)); what it reaches by reference, and every global, is shareable or a `const` object of frozen type ([`thread-share`](#thread-share)), or a local the function lends it and keeps off until the thread ends ([`thread-lent`](#thread-lent)); a locked member is used only while a guard on its lock lives ([`lock-held`](#lock-held)). Nothing of this changes what is compiled: these rules have no check behind them at run time.

```cpp accepted
struct Item { int id = 0; std::string name; };

class Inventory {
public:
  void add(Item item) { items_.push_back(std::move(item)); }
  const Item *find(int id) const {
    for (const Item &item : items_)
      if (item.id == id)
        return &item;                 // a borrow of what the caller lent
    return nullptr;
  }
  std::vector<std::string> names() const {
    std::vector<std::string> out;     // owns its strings
    for (const Item &item : items_)
      out.push_back(item.name);
    return out;
  }
private:
  std::vector<Item> items_;           // an owner
};
```

## fix

Not a rule: what an error's fix is, and how to apply them all ([RFC 0049](/rfcs/0049-errors-that-carry-their-fix/)). When the compiler can write what an error asks for, the note under the error says the text and where, and carries it as a fix-it (`-fdiagnostics-parseable-fixits` prints it; an editor offers it):

```text
slice.h:13:49: error: 'data_' points to one object and cannot be indexed or offset [weavec-safe:index-single]
slice.h:24:15: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(size_) data_'; 'size_' is its count in the call to 'memcmp' at line 19
```

A fix has a level:

- **certain**: it states what the function with the error already does: it keeps the parameter (`WEAVEC_ESCAPES`, [`escape`](#escape)), it returns the end of its own bound (`WEAVEC_COUNTED_BY(0)`, [`call-args`](#call-args)), a global is never assigned (`const`, [`thread-share`](#thread-share)), another declaration of the function says it ([`bad-annotation`](#bad-annotation)).
- **likely**: a fact about a declaration that every use the compiler read agrees with: a library call that pairs the pointer with a count, a comparison that bounds its index or the pointer, a byte tested against zero, constant indices only, a bound that flows in from what it is set from or out to where it is stored, a result that is always one parameter (`WEAVEC_LIFETIMEBOUND`). Each use is read inside the function it is in; the note says which one.
- **choice**: two facts disagree, or the edit is a design decision (`WEAVEC_ALIASED` on a class, [`owner-path`](#owner-path)). It is printed, never written.

A pointer member of a class of plain values (no destructor, copy, move, base or virtual function, members that are scalars, pointers and views) that stores a parameter is a view's: the first fix is `WEAVEC_BORROWED` on the member, the second `WEAVEC_ESCAPES` on the parameter. A fix is text in the source and nothing else: the rules check an annotation the compiler wrote as one a person wrote, and nothing at run time trusts it more.

`weavec-cc --weavec-fix` applies them over a project. Survey the build first (`-fweavec-safe-survey -fweavec-ledger=ledger/`), then:

```text
$ weavec-c++ --weavec-fix -j8 ledger/
weavec: fix: round 1: 212 edits in 9 files (171 certain, 41 likely), 2 withdrawn; 388 errors after
weavec: fix: round 2: nothing to apply
weavec: fix: marked 14 files WEAVEC_SAFE_FILE
weavec: fix: 214 edits in 11 files; 807 errors before, 388 after: 122 have a choice or a fix withdrawn, 266 have no fix
```

Or give it a compile command: `weavec-c++ --weavec-fix -- weavec-c++ -std=c++20 -c parser.cpp`. Each round writes the certain fixes and the likely ones every unit agrees on, on every declaration of the function; runs the rules again on the units an edit reached (their recorded commands, with `-fsyntax-only`); and takes out a likely fix that brought an error with no fix (or a round that broke a compile), which is not tried again. When a round applies nothing it marks the files left with no error `WEAVEC_SAFE_FILE`. It refuses a file that changed since its ledger was written. `--dry-run` prints the first round as a diff and writes nothing; `--level=certain` writes no likely fix; `--only=<dir>,...` limits the files it edits; `--no-mark` marks none. `weavec-fix/weavec-fix.json`, beside the ledgers, logs each fix, its evidence, and what was taken out and why; `--weavec-audit weavec-fix/ledger` prints what is left: the choices, most errors first, and the errors with no fix by file. Review the diff (`git diff`) as a person's: a likely bound is a fact the uses agree on, not one the compiler proved, and the audit lists each bound on a member that code which is not safe can write.

## index-single

A pointer without a bound points to one object: it is dereferenced (`*p`, `p->f`, `p[0]`) but not indexed or offset (`p[i]`, `p + 2`, `p++`).

```c rejected
int second(const int *p) { return p[1]; }
```

```c accepted
int second(const int *WEAVEC_COUNTED_BY(n) p, size_t n) {
  return n > 1 ? p[1] : 0;
}
```

Give the declaration a bound (`WEAVEC_COUNTED_BY(n)`, `WEAVEC_SIZED_BY(n)`, `WEAVEC_ENDED_BY(e)`, `WEAVEC_NULL_TERMINATED`), or index an array or an allocation. A struct's pointer field takes the same annotations, naming a sibling field.


_Fix_ ([`fix`](#fix)): the bound the uses agree on, on the pointer's declaration (a local's, on the parameter, member or result it comes from): `WEAVEC_COUNTED_BY(n)` when a library call or a loop's comparison counts it by `n`, `WEAVEC_ENDED_BY(e)` when it is compared with `e`, `WEAVEC_NULL_TERMINATED` when a byte of it is tested against zero, `WEAVEC_COUNTED_BY(k + 1)` when only `p[0]` to `p[k]` are read.
### In C++

As in C: a raw pointer without a bound points to one object and is not indexed or stepped (the difference and the comparison of two pointers are allowed). It has a bound when its declaration gives one, when it is an array or was made from one, when it is a container's `data()` (`std::vector`, `std::string`, `std::array`, `std::span`, `std::string_view`), `main`'s `argv`, or a local assigned only such pointers. A lambda's copy of a pointer points to one object: capture a `std::span`. An iterator a library class spells as a pointer (`std::array<T, N>::iterator`) is the library's.

```cpp rejected
int third(const int *p) { return p[2]; }
```

```cpp accepted
int third(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n) { return n > 2 ? p[2] : 0; }
int spanned(std::span<const int> s) { return s[2]; }
int summed(const std::vector<int> &v) {
  const int *p = v.data();
  int total = 0;
  for (std::size_t i = 0; i < v.size(); ++i)
    total += p[i];
  return total;
}
unsigned digits(const char *WEAVEC_ENDED_BY(end) p, const char *end) {
  unsigned value = 0;
  while (p < end)
    value = value * 10 + unsigned(*p++ - '0');
  return value;
}
class Slice {
public:
  Slice(const char *WEAVEC_COUNTED_BY(n) WEAVEC_ESCAPES data, std::size_t n)
      : data_(data), size_(n) {}
  char operator[](std::size_t i) const { return data_[i]; }
  const char *WEAVEC_COUNTED_BY(size_) data() const { return data_; }
private:
  const char *WEAVEC_COUNTED_BY(size_) data_;
  std::size_t size_;
};
```

A member's bound names a sibling member; a member function's result may be counted by a member of its class, as `data()` is above. Every declaration of a function, and every override of a virtual one, gives the same bounds.

An end is declared `WEAVEC_COUNTED_BY(0)`: a pointer to no element, compared and subtracted but never read.

```cpp rejected
struct Range {
  const int *WEAVEC_COUNTED_BY(n) data;
  std::size_t n;
  const int *WEAVEC_COUNTED_BY(0) end() const { return data + n; }
};
int past(const Range &r) { return *r.end(); }
```

## int-to-pointer

No integer becomes a pointer, other than the constant null.

```c rejected
int *at(unsigned long address) { return (int *)address; }
```

Keep the pointer itself, and an offset beside it if one is needed.

### In C++

The same.

```cpp rejected
int *at(unsigned long address) { return (int *)address; }
```

## pointer-cast

A pointer converts to another pointer type only when no pointer can be forged: memory that holds pointers converts to nothing but `void` and `const` bytes; a `void *` converts to no other type, but where an allocation made it (`struct node *n = malloc(sizeof *n)`); function pointers do not convert. An allocation that becomes a `T *` is sized by `T` when its size names a type (`malloc(sizeof *p)`, `calloc(n, sizeof(struct node))`), since its block is reused for the type its size names; bytes of a type without pointers may be any such type's (`char *b = malloc(sizeof(struct header) + n)`).

```c rejected
struct node { struct node *WEAVEC_OWNED next; int v; };
int peek(void *context) {
  struct node *n = context;
  return n->v;
}
```

```c accepted
struct node { struct node *WEAVEC_OWNED next; int v; };
int peek(const struct node *n) { return n->v; }
```

A library that hands back a `void *` context gets a `WEAVEC_GUARDED` adapter of a few lines that converts it and calls the safe function.

```c rejected
#include <stdlib.h>
struct node { struct node *WEAVEC_OWNED next; int v; };
struct wide { long a, b, c; };
struct node *WEAVEC_OWNED make(void) { return malloc(sizeof(struct wide)); }
```

```c accepted
#include <stdlib.h>
struct node { struct node *WEAVEC_OWNED next; int v; };
struct node *WEAVEC_OWNED make(void) {
  struct node *n = malloc(sizeof *n);
  return n;
}
```

### In C++

No `reinterpret_cast`, no `const_cast`, no C-style cast that is one of them, no `static_cast` from `void *` or from a base to a derived class. `dynamic_cast` and `std::variant` are the checked forms. One conversion is allowed ([RFC 0047](/rfcs/0047-conversion-in-place/)): a pointer to an object becomes a pointer to its bytes, `const char`, `const unsigned char` or `const std::byte`, and the same without `const` when the object is trivially copyable and holds no pointer; the bytes are the object's own.

```cpp accepted
struct Header { std::uint32_t magic; std::uint32_t size; };
std::string encode(const Header &h) {
  return std::string(reinterpret_cast<const char *>(&h), sizeof h);
}
```

```cpp rejected
struct Shape { virtual ~Shape() = default; };
struct Circle : Shape { double radius = 1; };
double radius(Shape *shape) { return static_cast<Circle *>(shape)->radius; }
```

```cpp accepted
struct Shape { virtual ~Shape() = default; };
struct Circle : Shape { double radius = 1; };
double radius(Shape *shape) {
  if (auto *circle = dynamic_cast<Circle *>(shape))
    return circle->radius;
  return 0;
}
```

Two conversions are rejected because what they make is used later ([RFC 0051](/rfcs/0051-nothing-undefined/)): a member pointer converted to a base class's (applied to a base that is not the derived object, it names a member that is not there), and an owner of a base (`std::unique_ptr<Base>`) made from an owner of a derived object when the base's destructor is not virtual (the object would be destroyed as the base). A writable byte view is not made of an object that holds a `bool` ([`pointer-bytes`](#pointer-bytes)).

```cpp rejected
struct Shape { double area = 0; };
struct Circle : Shape { std::string name = "c"; };
std::unique_ptr<Shape> make() { return std::make_unique<Circle>(); }
```

```cpp accepted
struct Shape { virtual ~Shape() = default; double area = 0; };
struct Circle : Shape { std::string name = "c"; };
std::unique_ptr<Shape> make() { return std::make_unique<Circle>(); }
```

## pointer-bytes

Memory that holds pointers is not written as bytes, nor copied or zeroed in part: a pointer would be half written or forged.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
void clear(struct item *it) { memset(it, 0, 4); }
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
void clear(struct item *it) {
  free(it->name);
  memset(it, 0, sizeof *it);
}
```

Copy whole objects (`*d = *s`), zero whole objects (`memset(p, 0, n * sizeof *p)`).

A `bool` is 0 or 1 ([RFC 0051](/rfcs/0051-nothing-undefined/)): bytes are not written into an object that holds one, by a library call that writes its argument (`memcpy`, `fread`, `read`, `recv` and the rest) or through a byte pointer, but a whole object zeroed or copied from one of its own type. Give the member the type `uint8_t`, or read the bytes and assign each member.

```c rejected
struct header { unsigned length; bool compressed; };
int load(FILE *file, struct header *h) { return fread(h, sizeof *h, 1, file) == 1; }
```

```c accepted
struct header { unsigned length; bool compressed; };
int load(FILE *file, struct header *h) {
  unsigned char bytes[8];
  if (fread(bytes, sizeof bytes, 1, file) != 1)
    return 0;
  h->length = (unsigned)bytes[0] | (unsigned)bytes[1] << 8;
  h->compressed = bytes[4] != 0;
  return 1;
}
```

### In C++

The bytes of an object that is not trivially copyable are not read or written by `memcpy`, `memset`, `memmove` or `memcmp`, and bytes are not written into an object that holds a `bool` or a pointer, by a call or by `std::bit_cast`: a pointer is made only from what it points to. The object a call writes is what its argument points to before it becomes the call's `void *`: an array's element when the array is passed by name, and through a cast to `void *`. A whole object zeroed (`memset` with 0, `bzero`), its elements reordered (`qsort`) or copied from an object of its own type is accepted, and a pointer may be read as an integer. A call that writes pointers of the library's making into an array its caller gives (`backtrace`, `epoll_wait`'s and `kevent`'s events) is rejected: make it in a `WEAVEC_GUARDED` function.

```cpp rejected
struct Item { int id = 0; std::string name; };
void copy(Item &to, const Item &from) { std::memcpy(&to, &from, sizeof(Item)); }
```

```cpp accepted
struct Item { int id = 0; std::string name; };
void copy(Item &to, const Item &from) { to = from; }
```

```cpp rejected
struct Ref { const long *p; };
Ref forge(std::uintptr_t bits) { return std::bit_cast<Ref>(bits); }
```

```cpp accepted
std::uintptr_t address(const long *p) { return std::bit_cast<std::uintptr_t>(p); }
```

```cpp rejected
bool flag(unsigned char byte) { return std::bit_cast<bool>(byte); }
```

```cpp accepted
bool flag(unsigned char byte) { return byte != 0; }
```

## union-pointer

A union member that holds a pointer is not read: the union may hold something else.

```c rejected
union value { long number; char *text; };
char first(union value *v) { return v->text[0]; }
```

Keep a tag beside a struct per variant, or one pointer per variant in a struct.

Nor is a `bool` member read where another member of another type shares its bytes ([RFC 0051](/rfcs/0051-nothing-undefined/)): the bytes may not be 0 or 1.

```c rejected
union flag { bool on; unsigned char byte; };
bool on(union flag *f) { return f->on; }
```

```c accepted
union flag { bool on; unsigned char byte; };
unsigned char byte(union flag *f) { return f->byte; }
```

### In C++

A union with a member that is a pointer, a view or an object with a destructor is not read in a safe function, and neither is a `bool` member that shares its bytes; `std::variant` keeps which alternative is live and checks it.

```cpp rejected
union Either { long number; const char *text; };
long number(Either e) { return e.number; }
```

```cpp accepted
using Either = std::variant<long, std::string>;
long number(const Either &e) { return std::holds_alternative<long>(e) ? std::get<long>(e) : 0; }
```

## escape

A pointer parameter is borrowed for the call: the function reads and writes through it and passes it on, but does not keep it after it returns. Storing it in memory other than a local, returning it, or passing it to a `WEAVEC_ESCAPES` parameter is an error.

```c rejected
struct holder { int *kept; };
void keep(struct holder *h, int *p) { h->kept = p; }
```

```c accepted
struct holder { int *kept; };
void keep(struct holder *h, int *WEAVEC_ESCAPES p) { h->kept = p; }
```

`WEAVEC_ESCAPES` after the `*` if the function keeps a reference (callers then cannot pass a stack address); `WEAVEC_LIFETIMEBOUND` after the parameter's name if the function returns it; `WEAVEC_OWNED` if the function takes the block and is responsible for releasing it (see `owner-source`); a `WEAVEC_BORROWED` field of a local struct to carry it down a call tree.


_Fix_ ([`fix`](#fix)): `WEAVEC_ESCAPES` on the parameter, or after the parameter list for `this`, on every declaration (certain); a returned parameter that every return gives, `WEAVEC_LIFETIMEBOUND` (likely); a pointer member of a class of plain values, `WEAVEC_BORROWED` first (likely).
### In C++

A borrow or a view of a local is not returned, and is not stored where it outlives the function's locals: in a member of `*this`, through a parameter, in a global. That covers a reference or a `std::string_view` of a local, a lambda that captures a local by reference, and a `std::vector` of views into a local buffer. A view member is assigned only from parameters that are themselves views (a `std::string_view`, a `std::span`, a pointer), which callers can see, or bound in the constructor; it is not made to point into another member of the same object, which would break when the object is moved. A `std::function`, a `std::thread` or a `std::any` made from a view is a view. A function that keeps a callable it was passed (in a member, a container, a global) takes it by value, `std::function<void()> handler`, which says so; one that only calls it takes `const std::function<void()> &` or a template parameter. (`WEAVEC_ESCAPES`, written after the type, says "kept" of a parameter taken another way, and of a pointer.) What is passed for a parameter that keeps is not a callable that captures a local by reference, nor the address of a local, unless the object that keeps it is itself a local of the same function, which then counts as a view of what the callable captured. A callable that is returned as a `std::function` owns what it captured.

A callable that is kept (in a member, an element, a `static`, by a parameter that keeps it) captures no reference, no `this` and no pointer or view ([RFC 0052](/rfcs/0052-what-a-call-may-reach/) §5.3): it captures values and owners, a `std::shared_ptr` or a `std::weak_ptr` included, so calling it later reaches nothing a borrow points into. For a class that derives from `std::enable_shared_from_this`, capture `[weak = weak_from_this()]` and lock it in the body; otherwise capture by value what the body uses, or take the object as the callable's argument. A callable a local of the same function keeps (a `Timer timer([&count] { … })`) is one the function sees: a call on the local may run it. A value whose members could hold a borrow of what it was made from (a struct with a pointer member, a `std::initializer_list`, a shared pointer made by the aliasing constructor, a range adaptor) is a view of it, and is not returned or kept past a local it borrows.

```cpp rejected
std::string read_line(int n);
std::string_view first_word(int n) {
  std::string line = read_line(n);
  return std::string_view(line).substr(0, line.find(' '));
}
```

```cpp rejected
struct Button {
  std::function<void()> on_click;
  void count_into(int &count) { on_click = [&count] { ++count; }; }
};
```

```cpp rejected
struct Document {
  std::string buffer;
  std::string_view title;
  void index() { title = std::string_view(buffer).substr(0, 8); }
};
```

```cpp rejected
struct Events {
  std::vector<std::function<void()>> handlers;
  void subscribe(const std::function<void()> &handler) { handlers.push_back(handler); }
};
```

```cpp rejected
struct Events {
  std::vector<std::function<void()>> handlers;
  void subscribe(std::function<void()> handler) { handlers.push_back(std::move(handler)); }
};
void count_events(Events &events) {
  int count = 0;
  events.subscribe([&count] { ++count; });
}
```

```cpp rejected
std::function<std::size_t()> length_of(const std::string &text) {
  return [&text] { return text.size(); };
}
```

```cpp rejected
struct Panel {
  int clicks = 0;
  std::function<void()> on_click = [this] { ++clicks; };
};
```

```cpp rejected
struct Span {
  const long *data;
  long size;
};
Span numbers() {
  long values[3] = {1, 2, 3};
  return Span{values, 3};
}
```

```cpp rejected
std::initializer_list<int> defaults() { return {1, 2, 3}; }
```

```cpp accepted
std::string read_line(int n);
std::string first_word(int n) {              // owns what it returns
  std::string line = read_line(n);
  return line.substr(0, line.find(' '));
}
std::string_view first_word_of(std::string_view line) {  // the caller keeps the line
  return line.substr(0, line.find(' '));
}
struct Button {
  std::function<void()> on_click;
  void count_into(std::shared_ptr<int> count) { on_click = [count] { ++*count; }; }
};
struct Panel {
  int clicks = 0;
  std::function<void(Panel &)> on_click = [](Panel &self) { ++self.clicks; };
};
struct Span {
  const long *data;
  long size;
};
Span numbers(const std::vector<long> &values) {   // the caller keeps the values
  return Span{values.data(), static_cast<long>(values.size())};
}
std::vector<int> defaults() { return {1, 2, 3}; }
struct Events {
  std::vector<std::function<void()>> handlers;
  void subscribe(std::function<void()> handler) { handlers.push_back(std::move(handler)); }
};
void count_events(Events &events, std::shared_ptr<int> count) {
  events.subscribe([count] { ++*count; });   // captures by value
}
int count_here() {
  int count = 0;
  Events events;                             // a local keeper: a view of 'count'
  events.subscribe([&count] { ++count; });
  for (const auto &handler : events.handlers)
    handler();
  return count;
}
std::function<std::size_t()> length_of(const std::string &text) {
  return [text] { return text.size(); };     // owns its copy
}
struct Document {
  std::string buffer;
  std::size_t title_length = 0;              // an offset and a length, not a view
  std::string_view title() const { return std::string_view(buffer).substr(0, title_length); }
};
```

A function keeps the address of what a pointer or reference parameter, or `this`, refers to only if it says so ([RFC 0048](/rfcs/0048-safe-code-without-the-shadow/)): storing it in a member, an element, a global or through another parameter, or handing it to a parameter that keeps (`push_back` of a container of pointers is one), needs `WEAVEC_ESCAPES` after the parameter's `*` or `&`, or after the member function's parameter list for `this`. A pointer or reference to a `WEAVEC_ALIASED` class keeps by declaration, and so does a template's own parameter type (`T item`, given a pointer by an instantiation), as a container keeps its elements. A caller's local whose address it is given then lives in typed memory for its scope: a pointer kept past the scope reaches memory of the local's type in a fast build, and stops with a report in the checked one. A constructor keeps the object it makes when it says `WEAVEC_ESCAPES` after its parameter list (a small buffer whose pointer points into the object itself) or when it makes a member or base that is kept; a class whose constructor makes one says so too, and its locals are placed, however they are made. A member's default initializer is the code of each constructor that does not name the member, and an implicit constructor whose default initializer points a member into the object keeps it. A store in a `WEAVEC_BORROWED` member, a view's, keeps nothing: the rules for views follow it. A temporary, even one a local reference extends, a parameter taken by value and a `thread_local` are never kept. Without the rule, a pointer member into a frame that returned could be written through when another frame is laid over it.

```cpp rejected
struct Counter { long hits = 0; };
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &c) { counters.push_back(&c); }
};
```

```cpp rejected
struct Listener {
  Listener *self = nullptr;
  void enroll() { self = this; }
};
```

```cpp accepted
struct Counter { long hits = 0; };
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &WEAVEC_ESCAPES c) { counters.push_back(&c); }
};
long count(Registry &registry) {
  Counter mine;                              // kept: placed in typed memory
  registry.add(mine);
  return mine.hits;
}
struct Listener {
  Listener *self = nullptr;
  void enroll() WEAVEC_ESCAPES { self = this; }
};
```

A raw pointer into storage that changes type in place is not kept in a member or an element either: an alternative of a `std::variant`, the characters of a `std::string` (whose bytes hold its buffer's pointer once it grows). In a fast build such a pointer, used after the change, would read one type as another with no check behind it. A pointer to `const` bytes without pointers (`s.c_str()`) may be kept, and so may a pointer to the variant or the string itself, or to an element of a container.

```cpp rejected
#include <string>
#include <variant>
struct Holder {
  std::string *name = nullptr;
  void keep(std::variant<std::string, int> &v) { name = &std::get<std::string>(v); }
};
```

Threads add three cases. A borrow of a locked member does not leave its lock: it is not returned or stored, since another thread may change the member as soon as the guard ends; return a copy. A thread object that holds a borrow is not detached, which would let it outlive what it borrowed; capture by value. And a class that owns a thread started with `this` declares the thread member after every other member, so that it is joined before they are destroyed; is `final` if it has virtual functions; cannot be copied or moved away from the thread; and is shareable ([`thread-share`](#thread-share)).

```cpp rejected
class Rows {
  std::mutex mu_;
  std::vector<std::string> rows_;
public:
  const std::string &first() {
    const std::lock_guard lock(mu_);
    return rows_.front();
  }
};
```

```cpp rejected
void report(const std::string &message) {
  std::string copy = message;
  std::thread t([&copy] { (void)copy.size(); });
  t.detach();
}
```

```cpp rejected
class Worker final {
  std::mutex mu_;
  std::jthread thread_;
  std::vector<std::string> names_;         // destroyed before the thread is joined
public:
  Worker() : thread_([this] { add(); }) {}
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  void add() {
    const std::lock_guard lock(mu_);
    names_.push_back("x");
  }
};
```

```cpp accepted
class Rows {
  std::mutex mu_;
  std::vector<std::string> rows_;
public:
  std::string first() {
    const std::lock_guard lock(mu_);
    return rows_.front();                  // a copy leaves the lock
  }
};
void report(const std::string &message) {
  std::thread t([copy = message] { (void)copy.size(); });
  t.detach();                              // the thread owns what it uses
}
class Worker final {
  std::mutex mu_;
  std::vector<std::string> names_;
  std::jthread thread_;                    // last: started last, joined first
public:
  Worker() : thread_([this] { add(); }) {}
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  void add() {
    const std::lock_guard lock(mu_);
    names_.push_back("x");
  }
};
```

```cpp accepted
#include <string>
#include <variant>
#include <vector>
struct Holder {
  const std::vector<std::variant<std::string, int>> *values = nullptr;
  std::size_t index = 0;                     // the key, looked up when it is used
  const std::string *name() const { return std::get_if<std::string>(&(*values)[index]); }
};
```

## call-args

An argument for a parameter with a bound has bounds that cover it; a pointer to one object is not passed, stored or returned where a declaration gives a bound.

```c rejected
int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
int one(const int *p) { return sum(p, 4); }
```

```c accepted
int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
int four(const int *WEAVEC_COUNTED_BY(4) p) { return sum(p, 4); }
```

Pass a pointer with bounds (an array, an allocation, a counted parameter), or declare the parameter with the same bound.

Nor does a C library function access, through a pointer to one object, more bytes than the object has ([RFC 0050](/rfcs/0050-checked-once/) §5.1): `memcpy(out, in, n)` needs both declared with their count. A count that stays inside the object (`memset(p, 0, sizeof *p)`) is accepted. What a string function reads is checked when it runs.

```c rejected
void copy(char *out, const char *in, size_t n) { memcpy(out, in, n); }
```

```c accepted
void copy(char *WEAVEC_COUNTED_BY(n) out, const char *WEAVEC_COUNTED_BY(n) in,
          size_t n) {
  memcpy(out, in, n);
}
```


_Fix_ ([`fix`](#fix)): the argument's bound, from its uses, on its declaration; for a stored or returned end of its own bound (`data_ + size_`), `WEAVEC_COUNTED_BY(0)` on where it goes (certain).
### In C++

The same for a function's, a constructor's or a member function's parameters with a bound, and for the library's that take a pointer and a count, or two pointers: `std::span(p, n)`, `std::string_view(p, n)`, `std::string(p, n)`, `append(p, n)`, `assign(p, n)`, `std::vector(first, last)`. Their pointer has a bound that covers the count, checked when it runs; in a fast unit a `std::span` or a `std::string_view` made from a pointer, or from an iterator, is checked once where it is made, since its index is checked by the library alone (RFC 0050 §2.3).

```cpp rejected
std::string_view view(const char *p, std::size_t n) { return std::string_view(p, n); }
```

```cpp accepted
std::string_view view(const char *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  return std::string_view(p, n);
}
```

A raw pointer without a bound points to one object (RFC 0048), so a position is not one: a pointer computed by arithmetic from a bounded one is not returned, stored or passed where a plain pointer is declared. Declare an end `WEAVEC_COUNTED_BY(0)`, a cursor `WEAVEC_ENDED_BY(limit)` or with a count, or take `&p[i]`, which is compared with the bound. A C library function's access is checked as in C, above.

```cpp rejected
class Buffer {
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
public:
  const char *end() const { return data_ + size_; }
};
void copy(char *WEAVEC_COUNTED_BY(n) out, const char *in, std::size_t n) {
  std::memcpy(out, in, n);
}
```

```cpp accepted
class Buffer {
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
public:
  const char *WEAVEC_COUNTED_BY(0) end() const { return data_ + size_; }
  const char *at(std::size_t i) const { return &data_[i]; }
};
void copy(char *WEAVEC_COUNTED_BY(n) out, const char *WEAVEC_COUNTED_BY(n) in, std::size_t n) {
  std::memcpy(out, in, n);
}
```

## variadic

A variadic call is only to a function of the library table whose row describes its format (`printf` is described); `va_start` is not used, nor `va_arg` of a type that holds pointers.

```c rejected
int total(int count, ...);
int call(void) { return total(2, 1, 2); }
```

Take the arguments by type: a counted array and its length.

### In C++

A safe function does not read variadic arguments with `va_start` and `va_arg`; a parameter pack, a `std::initializer_list` or a `std::span` carries them with their types. Calling `printf` is allowed, and its format is checked as in C.

```cpp rejected
int sum(int count, ...) {
  va_list arguments;
  va_start(arguments, count);
  int total = 0;
  for (int i = 0; i < count; ++i)
    total += va_arg(arguments, int);
  va_end(arguments);
  return total;
}
```

```cpp accepted
template <typename... T> int sum(T... values) { return (0 + ... + values); }
int sum_of(std::initializer_list<int> values) {
  int total = 0;
  for (int value : values)
    total += value;
  return total;
}
```

## no-asm

No inline assembly (an empty `asm volatile("" ::: "memory")` barrier is fine), `setjmp`, `longjmp`, `__builtin_frame_address` or computed `goto`.

```c rejected
int cycles(void) {
  int r;
  __asm__("mov %0, #1" : "=r"(r));
  return r;
}
```

Move it into a `WEAVEC_GUARDED` or `WEAVEC_UNSAFE` helper.

### In C++

The same.

```cpp rejected
int pause() {
  __asm__("nop");
  return 0;
}
```

## assumed

A safe function says nothing to the optimizer that nothing checks ([RFC 0051](/rfcs/0051-nothing-undefined/) §4.1): if the promise is wrong, the optimizer removes the checks that would have caught it. Not `[[assume(e)]]`, `__builtin_assume`, `__builtin_assume_aligned` or `__builtin_assume_separate_storage`; not `restrict` on a parameter or a local, nor `static` in an array parameter (`int a[static 4]`); not the attributes `pure`, `const`, `malloc`, `alloc_size`, `alloc_align`, `assume_aligned`, `returns_nonnull` or `nonnull` on the function, or (but the allocation attributes, which a project's own allocator states and Safe C types allocations by) on a function it calls that the unit's own files declare; not `#pragma clang loop vectorize(assume_safety)` or `interleave(assume_safety)`. The error's fix is certain: it deletes the token, or writes the form that asserts nothing, and `--weavec-fix` applies it. Hints that are only hints stay: `[[likely]]`, `__builtin_expect`, `[[nodiscard]]`, `noexcept`.

```c rejected
long sum2(long *restrict a, long *restrict b) { return *a + *b; }
```

```c accepted
long sum2(long *a, long *b) { return *a + *b; }
```

`if (!(e)) __builtin_unreachable();` states what `[[assume(e)]]` does and is checked: reaching it stops with `unreachable` ([`defined`](#defined)).

### In C++

The same, `[[assume]]` included; `if (!(e)) std::unreachable();` is the checked form.

```cpp rejected
long at(const std::array<long, 8> &cells, std::size_t i) {
  [[assume(i < 8)]];
  return cells[i];
}
```

```cpp accepted
long at(const std::array<long, 8> &cells, std::size_t i) { return cells[i]; }
```

## signal-handler

A safe function does not register code the system runs between any two instructions of the program ([RFC 0051](/rfcs/0051-nothing-undefined/) §4.3): `signal`, `sigaction`, `bsd_signal`, `sigset` and `sigvec` (the library table's `handler` rows). `signal` with `SIG_IGN` or `SIG_DFL` registers no code and is accepted, and so is `sigaction` that only reads the old action. Register the handler in a `WEAVEC_GUARDED` function and let it only set a `volatile sig_atomic_t` (C) or a `std::atomic` (C++), or write a byte to a pipe; or block the signal and wait for it with `sigwait` on a thread. The audit lists every function that is not safe and registers a handler.

```c rejected
volatile sig_atomic_t stops;
void on_stop(int signal) { (void)signal; stops = 1; }
void watch(void) { signal(SIGINT, on_stop); }
```

```c accepted
void quiet(void) { signal(SIGPIPE, SIG_IGN); }
```

### In C++

The same.

```cpp rejected
std::atomic<int> stops{0};
void onStop(int) { stops.fetch_add(1); }
void watch() { std::signal(SIGINT, onStop); }
```

## unsafe-block

A safe function holds no `WEAVEC_UNSAFE` block: every exemption is a function of its own, visible in a signature and in the audit.

```c rejected
int peek(int *p) {
  WEAVEC_UNSAFE { return p[1]; }
}
```

Move the block into a `WEAVEC_UNSAFE` function.

### In C++

The same.

```cpp rejected
int peek(const std::vector<int> &v) {
  WEAVEC_UNSAFE { return v[1]; }
}
```

## bad-annotation

A bound names a parameter (or, on a field, a sibling field) of the right type, an integer literal, or a macro that expands to one; every declaration of a function gives it the same bounds.

```c rejected
int sum(const int *WEAVEC_COUNTED_BY(count) v, size_t n);
```

```c accepted
int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
```


_Fix_ ([`fix`](#fix)): what one declaration of a function says, written on the other (certain).
### In C++

Safe C's bounds mean the same in C++. `WEAVEC_OWNED` and `WEAVEC_DROPS` are C's: an owner is a `std::unique_ptr` or a container, and a destructor releases what a class owns. `WEAVEC_ESCAPES`, `WEAVEC_BORROWED` and `WEAVEC_LIFETIMEBOUND` keep their meaning. `WEAVEC_SENDS` ([`thread-send`](#thread-send)) goes after the type of a parameter that is a type-erased callable, the same in every declaration of the function; `WEAVEC_ALIASED` ([`owner-path`](#owner-path)) after `class` or `struct`; in C both are errors. Every declaration of a function, and every function that overrides a virtual one, gives the same bounds.

```cpp rejected
void take(int *WEAVEC_OWNED p) { (void)p; }
```

```cpp rejected
void schedule(int WEAVEC_SENDS delay) { (void)delay; }
```

```cpp rejected
struct Reader {
  virtual ~Reader() = default;
  virtual std::size_t read(char *WEAVEC_COUNTED_BY(n) to, std::size_t n) = 0;
};
struct Zeros : Reader {
  std::size_t read(char *to, std::size_t n) override { (void)to; return n; }
};
```

```cpp accepted
int first(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n) { return n != 0 ? *p : 0; }
std::unique_ptr<int> take(std::unique_ptr<int> p) { return p; }
```

## counted-pair

A count or end that bounds a pointer field changes only in the statement next to its pointer's assignment (either order), or alone by a step (`n++`, `n += k`, `n--`, `n -= k`, `n = 0`), which is checked when it runs. A struct initializer gives its counted pointer at least the elements its count says.

```c rejected
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
void shrink(struct vec *v, size_t cap) { v->cap = cap; }
```

```c accepted
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
int grow(struct vec *v) {
  size_t cap = v->cap ? 2 * v->cap : 8;
  int *d = realloc(v->data, cap * sizeof *d);
  if (d == NULL)
    return -1;
  v->data = d;
  v->cap = cap;
  return 0;
}
```

Allocate into a local first, test it, then assign the pointer and its count in adjacent statements with no call between them.

### In C++

A member that bounds a pointer member is paired with it the same way, in a member function (`data_ = p; size_ = n;`) and in a constructor's list, where both are initialized, in any order. A count stepped down alone (`size_ -= n`) is checked when it runs.

```cpp rejected
class Slice {
public:
  void resize(std::size_t n) { size_ = n; }
private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};
```

```cpp accepted
class Slice {
public:
  Slice(const char *WEAVEC_COUNTED_BY(n) WEAVEC_ESCAPES data, std::size_t n)
      : data_(data), size_(n) {}
  void remove_prefix(std::size_t n) {
    data_ += n;
    size_ -= n;
  }
private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};
```

## string-index

A null-terminated pointer is read at its current byte and stepped forward by constants; an index or offset that is not a constant needs its length first.

```c rejected
char at(const char *WEAVEC_NULL_TERMINATED s, size_t i) { return s[i]; }
```

```c accepted
char at(const char *WEAVEC_NULL_TERMINATED s, size_t i) {
  size_t n = strlen(s);
  return i < n ? s[i] : 0;
}
```

`size_t n = strlen(s);` gives `s` the bounds `[s, s + n + 1)` while `s` is not assigned again. Step a string inside the branch that tested the bytes stepped over.

### In C++

The same for a `WEAVEC_NULL_TERMINATED` pointer; a `std::string`'s `c_str()` is one. A `std::string_view` or a `std::string` made from the pointer is the C++ way to index it.

## string-write

Nothing is stored through a null-terminated pointer, nor passed where the callee may write through it: a string is read-only in safe code. A buffer that is written is counted. In C++ the same holds for a `WEAVEC_NULL_TERMINATED` pointer; a `std::string`'s `data()` is counted, not a string, and may be written.

```c rejected
void trim(char *WEAVEC_NULL_TERMINATED s) { s[0] = 0; }
```

```c accepted
void trim(char *WEAVEC_COUNTED_BY(cap) s, size_t cap) {
  if (cap > 0)
    s[0] = 0;
}
```

## field-address

The address of a pointer field (or of an element of an array of them) does not leave the function: as an argument, a result, or a value stored in memory.

```c rejected
struct node { struct node *WEAVEC_OWNED next; };
struct node *WEAVEC_OWNED *link(struct node *n) { return &n->next; }
```

Pass the struct that holds the field, or read the field into a local, pass that, and assign the field back.

### In C++

The address of a pointer member with a bound, or of its count, and a reference that is not `const` to either, does not leave the function and is not passed for a reference parameter: through it one would change without the other. Swap or assign the whole object, or the two members as a pair.

```cpp rejected
class Slice {
public:
  void swap_size(Slice &other) { std::swap(size_, other.size_); }
private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};
```

```cpp accepted
class Slice {
public:
  void swap(Slice &other) { std::swap(*this, other); }
  std::size_t size() const { return size_; }
private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};
```

## owner-moved

A pointer declared `WEAVEC_OWNED` is the one responsible for releasing its block. An owner is not used after it was moved (passed to a `WEAVEC_OWNED` parameter, returned, stored in another owner) or released, on any path that reaches the use; nor before the result of `realloc` that took it was tested. A struct is not passed, moved or left behind at a return while an owner in it that the function moved out was not assigned again.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
void item_free(struct item *WEAVEC_OWNED it);
int last(struct item *WEAVEC_OWNED it) {
  item_free(it);
  return it->v;
}
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
void item_free(struct item *WEAVEC_OWNED it);
int last(struct item *WEAVEC_OWNED it) {
  int v = it->v;
  item_free(it);
  return v;
}
```

Use the owner before you give it away, or keep what you need in a local. An owner field moved out of a struct the caller owns (`free(v->data)`) is assigned before the function returns (`v->data = NULL`). Assigning a moved owner (`p = NULL`) makes it usable again.

### In C++

An object with a destructor to run is moved when `std::move` hands it to a parameter taken by value or by rvalue reference, or to another object. After that it may be assigned or destroyed; any other use is rejected, on every path that reaches it, `clear()` and `size()` included. A member moved out of `*this`, or out of a parameter taken by reference, is assigned again before the function returns: `std::exchange(member, {})` moves and leaves an empty object in one step. `std::move` of a scalar is a copy and moves nothing.

```cpp rejected
struct Job { std::string name; };
void run(std::unique_ptr<Job> job);
std::size_t submit(std::unique_ptr<Job> job) {
  run(std::move(job));
  return job->name.size();
}
```

```cpp accepted
struct Job { std::string name; };
void run(std::unique_ptr<Job> job);
std::size_t submit(std::unique_ptr<Job> job) {
  const std::size_t length = job->name.size();   // read before the move
  run(std::move(job));
  return length;
}
struct Queue {
  std::vector<Job> jobs;
  std::vector<Job> take() { return std::exchange(jobs, {}); }
};
```

## owner-leak

An owner holds no block when it is dropped: when its scope ends or the function returns, when it is assigned over, when the struct that holds it is released, and when a call's owned result is discarded or stored where it is only a reference. "Holds no block" means: moved or released on every path, or tested null. `exit` and `abort` drop nothing.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
struct item *WEAVEC_OWNED item_new(int v);
void item_free(struct item *WEAVEC_OWNED it);
int check(int c) {
  struct item *it = item_new(c);
  if (c > 100)
    return -1;
  item_free(it);
  return 0;
}
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
struct item *WEAVEC_OWNED item_new(int v);
void item_free(struct item *WEAVEC_OWNED it);
int check(int c) {
  struct item *it = item_new(c);
  int r = c > 100 ? -1 : 0;
  item_free(it);
  return r;
}
```

A destructor releases each owner field before the struct (`free(it->name); free(it);`). A struct the caller keeps (a local, a field) is released by a function whose parameter is `WEAVEC_DROPS` (`void vec_free(struct vec *WEAVEC_DROPS v)`): after any other call given a pointer to it, the struct's owners may hold blocks and its owner releases them before it goes out of scope; after that call they are null, and the function is checked to leave them so. A setter releases the old block before it stores the new one. `p = realloc(p, n)` loses the block when `realloc` fails: assign the result to a new local, test it, then assign `p`. A local that receives an allocation (`char *p = malloc(n)`) is an owner without being declared one; a function that returns one declares its result `WEAVEC_OWNED`.

## owner-source

Only an owner's value goes into an owner (a field, a parameter or a result declared `WEAVEC_OWNED`, or an owner local): the result of a call that returns one, another owner (which moves it), or null. Not a borrowed parameter, a borrow or a reference, the address of a local or a global, a string literal, or a pointer computed by arithmetic. An owner is not stepped (`p++`), and a block is released only through its owner.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
void set_name(struct item *it, char *name) {
  free(it->name);
  it->name = name;
}
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
void set_name(struct item *it, char *WEAVEC_OWNED name) {
  free(it->name);
  it->name = name;
}
```

Take the parameter `WEAVEC_OWNED` if the function keeps the block, or store a copy (`strdup(name)`). Step a borrow (`char *q = p; q++;`), not the owner. A struct that holds owners is assigned as a struct (`*a = *b`, which moves them), not copied as bytes.

## owner-path

An owner is moved, released or replaced only along the path that owns it: from a local owner, or from a parameter that is not a pointer to `const`, through owners. Not through a reference (a pointer field that is not `WEAVEC_OWNED`), not through a pointer to `const`, and not below a global read in place. A reference is not passed where the callee may change the owners below it.

```c rejected
struct node { struct node *WEAVEC_OWNED next; struct node *prev; int v; };
void node_free(struct node *WEAVEC_OWNED n);
void drop_next(struct node *n) {
  struct node *WEAVEC_OWNED next = n->prev->next;
  n->prev->next = NULL;
  node_free(next);
}
```

```c accepted
struct node { struct node *WEAVEC_OWNED next; struct node *prev; int v; };
void node_free(struct node *WEAVEC_OWNED n);
void drop_next(struct node *n) {
  struct node *WEAVEC_OWNED next = n->next;
  n->next = NULL;
  node_free(next);
}
```

Reach the owner from the pointer that owns it (a list's head, a tree's root), or keep the function `WEAVEC_GUARDED`. Move a global owner into a local first (`T *WEAVEC_OWNED s = g; g = NULL;`), change it, and move it back.


_Fix_ ([`fix`](#fix)): a choice, `WEAVEC_ALIASED` on the class, which the compiler prints and never writes.
### In C++

What an object owns is changed only from what owns it or was lent it: a local, a parameter, `this`, and through members, owners (`std::unique_ptr`, containers) and borrows. Through a raw pointer kept in a member or in a container's element, or through a `std::shared_ptr`, a safe function reads, calls `const` member functions and assigns members that own nothing; it changes what an object owns only when the object's class is declared `WEAVEC_ALIASED` (or derives from such a class), is shareable (it locks its members), or is a library type with no storage that changes type in place (a `std::vector<int>`; not a `std::string` or a `std::variant`). A fresh local `std::shared_ptr` from `std::make_shared` that is only dereferenced and moved is the function's own until it is published. That is what lets a borrow survive a call that was not handed its owner: no callee can reach the owner through a pointer it kept, unless the class says it can. A member function that is `const`, or has a `const` twin, changes nothing `*this` owns, `mutable` members included.

`WEAVEC_ALIASED` goes after `class` or `struct`. An aliased class owns no `std::variant`, `std::any` or `std::function`, and no object of a polymorphic class that is not aliased itself, at any depth: changing such storage in place under a borrow reads one type as another, which no check at run time can catch. A string's characters are the exception, which [`stale-borrow`](#stale-borrow) covers. What a borrow points into in an aliased object is not followed across a call that may reach the object: a use after such a change is found when the checked build (`-fweavec-safe-checks=full`) runs, and the audit lists the aliased classes.

```cpp rejected
struct Doc { std::variant<std::vector<long>, std::string> data; };
void clobber(const std::shared_ptr<Doc> &doc) { doc->data = std::string("A"); }
```

```cpp rejected
struct WEAVEC_ALIASED Doc { std::variant<std::vector<long>, std::string> data; };
```

```cpp accepted
class WEAVEC_ALIASED Env {
public:
  void open(const std::string &name) { names_.push_back(name); }
private:
  std::vector<std::string> names_;
};
struct Table {
  Env *env = nullptr;
  std::shared_ptr<Env> shared;
  void load() { env->open("a"); shared->open("b"); }
};
struct Doc { std::variant<std::vector<long>, std::string> data; };
void replace(std::shared_ptr<Doc> &doc) { doc = std::make_shared<Doc>(); }
```

Without the declaration, the fixes are as before: own the object; take it as a parameter; make the member a reference, which makes the class a view that lives in a local.

What an owning pointer (`std::unique_ptr`, an owner whose `const` access returns a reference that is not `const`) holds is written only through a path that is not `const`: in a `const` member function, `impl_->count++` writes what `impl_` owns, so two threads reading the object at once would race ([RFC 0046](/rfcs/0046-thread-safety/), D4). A raw pointer member is not an owning pointer: writing through one in a `const` member function stays allowed, which is why a class with one is not frozen.

```cpp rejected
struct Counter { int hits = 0; };
class Widget {
  std::unique_ptr<Counter> impl_ = std::make_unique<Counter>();
public:
  void touch() const { impl_->hits++; }
};
```

```cpp rejected
struct Subject;
struct Observer {
  Subject *subject = nullptr;
  void done();
};
struct Subject {
  std::vector<Observer *> observers;
  void remove(Observer *observer);
};
void Observer::done() { subject->remove(this); }   // while the subject iterates its observers
```

```cpp accepted
struct Subject;
struct Observer {
  bool finished = false;
  void done() { finished = true; }                  // the subject removes it afterwards
};
struct Subject {
  std::vector<Observer *> observers;
  void sweep() { std::erase_if(observers, [](const Observer *o) { return o->finished; }); }
};
struct Counter { int hits = 0; };
class Widget {
  std::unique_ptr<Counter> impl_ = std::make_unique<Counter>();
public:
  void touch() { impl_->hits++; }                   // not 'const': it says it writes
  int hits() const { return impl_->hits; }
};
struct Node {
  std::vector<std::unique_ptr<Node>> children;
  Node *parent = nullptr;                           // read, never changed through
  int depth() const { return parent != nullptr ? parent->depth() + 1 : 0; }
  void add(std::unique_ptr<Node> child) WEAVEC_ESCAPES {   // keeps 'this'
    child->parent = this;
    children.push_back(std::move(child));           // changed from its owner
  }
};
```

## owner-slot

An owner the function does not name exactly (an element at an index, `*pp`, a field reached through a borrow) is moved out of only in a statement followed at once by an assignment to the same place, so that no owner is left holding a pointer it gave away.

```c rejected
struct bag { char *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(len) items; size_t len; };
void drop(struct bag *b, size_t i) { free(b->items[i]); }
```

```c accepted
struct bag { char *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(len) items; size_t len; };
void drop(struct bag *b, size_t i) {
  free(b->items[i]);
  b->items[i] = NULL;
}
```

`x = a[--n]; a[n] = NULL;` and a swap through a local owner are accepted.

## stale-borrow

A borrow (a local pointer taken from an owner or from inside what it owns) is not used after that owner, or one above it, was moved, released or replaced by the function, nor after a call that could have done so: a call given a pointer to non-`const` memory that holds owners.

```c rejected
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
int vec_grow(struct vec *v);
int first_after_grow(struct vec *v) {
  int *first = v->data;
  if (vec_grow(v))
    return -1;
  return first[0];
}
```

```c accepted
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
int vec_grow(struct vec *v);
int first_after_grow(struct vec *v) {
  if (vec_grow(v))
    return -1;
  int *first = v->data;
  return v->cap > 0 ? first[0] : 0;
}
```

Take the pointer again after the call, keep an index instead, or declare the callee's parameter `const` if it changes no owner.

### In C++

A borrow (a reference, a pointer, an iterator, a `std::string_view`, a `std::span`) or a view is not used after an owner it points into

- was destroyed: the end of a local's scope, the end of the statement for a temporary;
- was moved or assigned;
- was changed: a member function that is not `const` and has no `const` twin was called on it (`push_back`, `erase`, `clear`, `reset`, `operator[]` of a map, which inserts), or it was passed to a function through a reference or pointer that is not `const`;
- was changed through another borrow of it: a reference, a pointer, a `std::reference_wrapper`'s `get()`, a span's element, a call's result;
- could have been reached by a call ([RFC 0052](/rfcs/0052-what-a-call-may-reach/)): the call was given a view that points into it (a `std::span<T>`, a struct that holds a pointer or a reference), whatever the view's own `const`; or it was a member function of such a view of the program's class (`m.flip()`, where `m` holds a reference to the owner);
- was changed by a lambda the function can see, where the lambda is called and at every call it is passed to (`std::visit`, `std::invoke`, an algorithm, a function of the program).

A reference or pointer a function returns points into every argument whose type can hold what it names, and a member function's into its object as well: `char &first(std::string &s)` returns into `s`; `std::map::find(key)` returns into the map, not into a `std::string` key. When a function returns into one parameter only, say so with `WEAVEC_LIFETIMEBOUND` on it (on the function, after its `const`, for `this`); the fixer proposes it when every `return` of a function in the unit gives a borrow of that parameter.

The checker does not know which element a borrow points to, so a change of the container ends every borrow into it. Changing an element (`item.name += "x"`) ends borrows into that element's own data, not the loop over the container. Take the borrow again after the change, keep an index instead, or make the callee's parameter `const` when it changes nothing.

```cpp rejected
int first_after_growing(std::vector<int> &v) {
  int &first = v[0];
  v.push_back(1);
  return first;
}
```

```cpp rejected
std::string make_name(int n);
std::size_t length(int n) {
  std::string_view name = make_name(n);   // the temporary string ends here
  return name.size();
}
```

```cpp rejected
void drop_negative(std::vector<int> &v) {
  for (int x : v)
    if (x < 0)
      v.erase(v.begin());
}
```

```cpp rejected
char &first(std::string &s) { return s[0]; }
void grow(std::string &s) {
  char &c = first(s);
  s.append(100, 'g');
  c = 'X';
}
```

```cpp rejected
using Value = std::variant<std::string, long>;
void reset(Value &v) {
  auto to_number = [&v] { v = 0L; };
  std::string &text = std::get<std::string>(v);
  to_number();
  text += "!";
}
```

```cpp rejected
using Value = std::variant<std::string, long>;
struct Setter {
  Value &target;
  void clear() const { target = 0L; }
};
void reset(Value &v) {
  const Setter setter{v};
  std::string &text = std::get<std::string>(v);
  setter.clear();                          // reaches what 'setter' refers to
  text += "!";
}
```

```cpp accepted
int first_after_growing(std::vector<int> &v) {
  v.push_back(1);
  int &first = v[0];                       // taken after the change
  return first;
}
std::string make_name(int n);
std::size_t length(int n) {
  std::string name = make_name(n);         // a local keeps the string
  std::string_view view = name;
  return view.size();
}
void drop_negative(std::vector<int> &v) {
  for (auto it = v.begin(); it != v.end();)
    it = *it < 0 ? v.erase(it) : it + 1;   // erase returns the next iterator
}
void rename_all(std::vector<std::string> &names) {
  for (std::string &name : names)
    name += "!";                           // changes an element, not the vector
}
char &first(std::string &s) { return s[0]; }
void grow(std::string &s) {
  s.append(100, 'g');
  char &c = first(s);                      // taken after the change
  c = 'X';
}
const std::string &longer(const std::string &a WEAVEC_LIFETIMEBOUND,
                          const std::string &b);
std::size_t keep(std::string &a, std::string &b) {
  const std::string &l = longer(a, b);
  b += "!";                                // 'l' borrows from 'a' only
  return l.size();
}
```

A string's characters move when the string grows, and a string is in nearly every class, aliased ones included. So a borrow through which the characters of a `std::string` the function does not own can be written (a `char *`, a `char &`, a `std::span<char>`; not its own local, and not a `const` borrow) ends at every call of the program's own functions, and at every change through a pointer kept in memory or a `std::shared_ptr` ([RFC 0047](/rfcs/0047-conversion-in-place/)).

```cpp rejected
void flush();
void fill(std::string &name) {
  char *p = name.data();
  flush();
  *p = 'x';
}
```

```cpp accepted
void flush();
void fill(std::string &name) {
  flush();
  char *p = name.data();                   // taken after the call
  *p = 'x';
}
```

Threads add two ends. A borrow of a locked member points into the guard as well ([`lock-held`](#lock-held)), and ends when the guard is destroyed or unlocked, or is handed by reference to a call that may give the lock away for a while, as a condition variable's `wait` does. And a thread object's destruction joins its thread, so it is a use of everything the thread borrowed: a local the thread uses ends after the thread object, or the thread is joined first.

```cpp rejected
class Inbox {
  std::mutex mu_;
  std::condition_variable ready_;
  std::vector<std::string> items_;
public:
  std::size_t first_length() {
    std::unique_lock lock(mu_);
    const std::string &first = items_.front();
    ready_.wait(lock);                     // gives the lock away for a while
    return first.size();
  }
};
```

```cpp rejected
void outlives() {
  std::jthread worker;
  {
    std::string message = "hi";
    worker = std::jthread([&message] { (void)message.size(); });
  }                                        // 'message' ends, the thread runs on
}
```

```cpp accepted
class Inbox {
  std::mutex mu_;
  std::condition_variable ready_;
  std::vector<std::string> items_;
public:
  std::size_t first_length() {
    std::unique_lock lock(mu_);
    ready_.wait(lock, [this] { return !items_.empty(); });
    return items_.front().size();          // taken after the wait
  }
};
void joined_first() {
  std::string message = "hi";
  std::jthread worker([&message] { (void)message.size(); });
}                                          // the thread is joined before 'message' ends
```

## call-alias

In one call, an argument through which the callee could change owners does not point to the same object as another argument that holds owners, nor above or below it, as far as the function knows where both came from.

```c rejected
struct item { char *WEAVEC_OWNED name; };
struct bag { struct item *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(len) items; size_t len; };
void bag_put(struct bag *b, struct item *it);
void again(struct bag *b) {
  if (b->len > 0)
    bag_put(b, b->items[0]);
}
```

Pass only the pointer the callee changes, an index into it, or make the other parameter `const`.

### In C++

In one call of a function of the program, an argument through which the callee could change what an object owns (a reference or pointer that is not `const`, the object of a member function that is not `const`) is not passed beside a borrow into the same object. The standard library's own functions are trusted with such arguments (`v.push_back(v[0])`, `std::sort(v.begin(), v.end())`).

A callable that may change an object (a lambda the function can see that changes what it captured, or `*this`) is not passed beside that object or a borrow into it, to any function: `f(v, [&v] { … })`, `std::visit([&v](auto &x) { … }, v)`, `obj.visit([&obj] { … })` ([RFC 0052](/rfcs/0052-what-a-call-may-reach/) §5.2). In exchange, a function believes that a callable it is given reaches nothing its other parameters and `this` reach, so a visitor that loops over its own container and calls the callable it was lent is accepted. Collect the changes in a local and apply them after the call, or let the callable return what to change.

```cpp rejected
void append(std::vector<std::string> &to, const std::string &s);
void repeat_first(std::vector<std::string> &v) { append(v, v[0]); }
```

```cpp rejected
void duplicate(std::vector<int> &v) {
  std::for_each(v.begin(), v.end(), [&](int x) { v.push_back(x); });
}
```

```cpp rejected
using Value = std::variant<std::string, long>;
void each_word(Value &v, const std::function<void(std::string &)> &fn);
void count(Value &v) {
  each_word(v, [&v](std::string &) { v = 0L; });
}
```

```cpp accepted
void append(std::vector<std::string> &to, const std::string &s);
void repeat_first(std::vector<std::string> &v) {
  const std::string first = v[0];          // a copy the callee cannot reach
  append(v, first);
}
void duplicate(std::vector<int> &v) {
  const std::vector<int> copy = v;
  v.insert(v.end(), copy.begin(), copy.end());
}
using Value = std::variant<std::string, long>;
void each_word(Value &v, const std::function<void(std::string &)> &fn);
void count(Value &v) {
  long words = 0;
  each_word(v, [&words](std::string &) { ++words; });
  v = words;                               // changed after the call
}
```

## raw-owner

Safe C++ only. A block is owned by an object, never by a raw pointer: no `new`, no `delete`, no explicit destructor call, no `malloc`, `free` or `strdup`, no `release()` of a smart pointer, no smart pointer made from a raw pointer. A `new` whose block a smart pointer takes in the same expression has its owner at once and is allowed when the pointer deletes it as it was made: `std::unique_ptr<T>(new T(...))`, `std::unique_ptr<T[]>(new T[n])`, `p.reset(new T)`; a derived object only through a base whose destructor is virtual. A `std::string` or a container made from a raw pointer copies what it points to and is not an owner of it. `std::make_unique`, `std::make_shared` and the containers make blocks with their owners, and destructors release them, so a double release, a mismatched `delete` and a forgotten release cannot be written. A class that must manage memory itself (a container, an arena, a wrapper of a C library's handle) is `WEAVEC_GUARDED`: it is the implementation of an owner, and safe code uses it as it uses the standard library's.

```cpp rejected
struct Item { int id = 0; };
Item *make_item() { return new Item; }
```

```cpp accepted
struct Item { int id = 0; };
std::unique_ptr<Item> make_item() { return std::make_unique<Item>(); }
class WEAVEC_GUARDED File {                // owns a C library's handle
public:
  explicit File(const char *path) : handle_(std::fopen(path, "r")) {}
  ~File() { if (handle_ != nullptr) std::fclose(handle_); }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  bool open() const { return handle_ != nullptr; }
private:
  std::FILE *handle_;
};
```

## not-modeled

A construct the rules do not follow is rejected, not guessed at. In C, a call that starts a thread (`pthread_create`, `thrd_create`, `dispatch_async`): Safe C does not check what a thread is given, so the function that starts one is `WEAVEC_GUARDED`, which the audit lists ([RFC 0046](/rfcs/0046-thread-safety/) §10). In C++, a coroutine, `goto`, a GNU statement expression, a function that returns twice (`setjmp`), and of threads: the same C calls (start the thread with `std::jthread`), an algorithm given a parallel execution policy, an OpenMP directive, a guard constructed with a tag (`std::defer_lock`, `std::try_to_lock`, `std::adopt_lock`) or moved, a lock handed to a function that is not a guard's constructor (`std::lock`), and a guard's member function the table does not name (`try_lock`). Write it another way, or keep the function `WEAVEC_GUARDED`.

```cpp rejected
void both(std::mutex &a, std::mutex &b) {
  std::unique_lock first(a, std::defer_lock);
  std::unique_lock second(b, std::defer_lock);
}
```

```cpp rejected
int count_down(int n) {
again:
  if (--n > 0)
    goto again;
  return n;
}
```

```cpp accepted
int count_down(int n) {
  while (--n > 0) {
  }
  return n;
}
void both(std::mutex &a, std::mutex &b) {
  const std::scoped_lock locks(a, b);       // takes both, in a safe order
}
```

## thread-send

Safe C++ only. What a thread start gives the new thread by value is sendable: each argument of a `std::thread` or `std::jthread` constructor and of `std::async`, and each by-copy capture of the callable. A sendable value carries nothing a thread could reach behind another's back: it owns what it holds, and every pointer in it points to something shareable. A `std::shared_ptr<T>` is sendable when `T` is shareable, or is `const` and frozen: share a class that locks its members, or a `std::shared_ptr<const T>` made by `std::make_shared<const T>`. A raw pointer member makes its struct sendable only if it points to something shareable.

A `std::function` does not say what it captured, so it crosses only from where that is known: a lambda written at the thread start, a local made from a lambda or a function and never assigned, or a parameter declared `WEAVEC_SENDS` (written after the type). A function that keeps a callable to run on another thread (a pool's `submit`) takes it that way, and a member of erased type in a locked class is given only such callables; a caller then passes a callable that captures nothing by reference and only what may be sent by value.

```cpp rejected
struct State { std::map<int, std::string> names; };
void start() {
  auto state = std::make_shared<State>();
  std::jthread a([state] { state->names[1] = "a"; });
}
```

```cpp rejected
void run(std::function<void()> task) {
  std::jthread worker(task);
}
```

```cpp accepted
struct State { std::map<int, std::string> names; };
class Names {
  std::mutex mu_;
  std::map<int, std::string> names_;
public:
  void set(int key, std::string name) {
    const std::lock_guard lock(mu_);
    names_[key] = std::move(name);
  }
};
void start() {
  auto snapshot = std::make_shared<const State>();
  std::jthread a([snapshot] { (void)snapshot->names.size(); });
  auto names = std::make_shared<Names>();
  std::jthread b([names] { names->set(1, "a"); });
}
void run(std::function<void()> WEAVEC_SENDS task) {
  std::jthread worker(std::move(task));
}
```

## thread-share

Safe C++ only. What a thread reaches by reference is shareable, an immutable object (a `const` local, or what a `std::shared_ptr<const T>` points to, of frozen type), or a local the function lends ([`thread-lent`](#thread-lent)): the target of a capture by reference, `this`, a pointer or a view passed or captured by value, a `std::ref`. A parameter is not lent, since the caller may use it meanwhile; neither is `*this` unless its class is shareable: share the object as a `std::shared_ptr<const T>`, start the threads in the function that owns it, or give the class a lock.

Every thread can reach a global, a static member and a function's `static` local, so each is shareable, `const` with a frozen type (or a constant whose initializer points only at constants), or `thread_local`. This holds in a program that starts no thread, as Rust's rule for `static` does; a global that code which is not safe trusts is marked `WEAVEC_GUARDED`, and the audit lists the safe functions that name it. A shareable global is used in place: calling a member function of a locked class on one is how it is meant to be used.

A `std::shared_ptr<const T>` is made with `std::make_shared<const T>`, or from a `std::shared_ptr<T>` no other handle shares (one `std::make_shared<T>` or `std::allocate_shared<T>` returned, or a local made so and only dereferenced before it is moved); one converted from a handle that still writes would let a reader race the writer. And a C function the table marks as keeping state of its own between calls (`strtok`, `localtime`, `rand`, `strerror`, `setenv`, `putenv`) is not called: use the reentrant form, `<random>` or `<chrono>`.

```cpp rejected
static std::vector<std::string> Log;
```

```cpp rejected
class Worker {
  std::vector<std::string> names_;
public:
  void start() {
    std::jthread t([this] { names_.push_back("x"); });
  }
};
```

```cpp rejected
void scan(const std::vector<int> &data) {
  std::jthread reader([&data] { (void)data.size(); });
}
```

```cpp accepted
thread_local std::vector<std::string> Log;
const std::vector<int> Primes{2, 3, 5};
static std::atomic<int> Requests{0};
class Worker {
  std::mutex mu_;
  std::vector<std::string> names_;
public:
  void add(std::string name) {
    const std::lock_guard lock(mu_);
    names_.push_back(std::move(name));
  }
  void start() {
    std::jthread t([this] { add("x"); });   // 'Worker' locks its members
  }
};
void scan(std::shared_ptr<const std::vector<int>> data) {
  std::jthread reader([data] { (void)data->size(); });
}
void readers() {
  const std::vector<int> data{1, 2, 3};
  std::jthread a([&data] { (void)data.size(); });
  std::jthread b([&data] { (void)data.front(); });
}
```


_Fix_ ([`fix`](#fix)): `const` on a global of the unit, or a static local, that is initialized by a constant and never assigned or bound to what is not `const` (certain).
## thread-lent

Safe C++ only. A local of the function (not a parameter, not a reference) that a thread borrows is **lent** to the thread object: to read, when its type is frozen and the thread only reads it (the lambda uses it only as `const`, a `std::cref`, a pointer to `const`, a `std::string_view`), else to write. While the thread object holds the loan, the function does not use a local lent to write at all (nor through a reference or a view of it), and does not write, move or lend to write a local lent to read. The loan ends when the thread object is destroyed, when it is joined (`join`, and `get` or `wait` of a `std::async` future), when a container of thread objects is destroyed or cleared, or after a loop that does nothing but join each of its threads (`for (auto &t : threads) t.join();`).

Threads that each write their own element of one container are lent the element at their own index, when the lambda uses the container only as `v[i]` with `i` a by-copy capture of the counter of the loop that starts them, and nothing else in the loop changes the counter: Rust's `chunks_mut` in C++'s spelling. Anything else that two threads do to one local, `std::vector<bool>`'s packed elements included, gives each thread its own data and collects the results.

```cpp rejected
void two_writers() {
  std::vector<int> v;
  std::jthread a([&v] { v.push_back(1); });
  std::jthread b([&v] { v.push_back(2); });
}
```

```cpp rejected
std::size_t used_meanwhile() {
  std::vector<int> v;
  std::jthread t([&v] { v.push_back(1); });
  return v.size();
}
```

```cpp accepted
std::size_t scoped_worker() {
  std::vector<std::string> lines{"a", "bb"};
  std::size_t total = 0;
  {
    std::jthread worker([&total, &lines] {
      for (const std::string &line : lines)
        total += line.size();
    });
  }                                           // the thread ends here
  return total + lines.size();                // and both locals are the function's again
}
int slots() {
  std::vector<int> parts(4);
  std::vector<std::jthread> workers;
  for (int i = 0; i < 4; ++i)
    workers.emplace_back([&parts, i] { parts[i] = i * i; });
  for (std::jthread &worker : workers)
    worker.join();
  return parts[3];
}
```

## lock-held

Safe C++ only. In a class with exactly one lock member (`std::mutex`, `std::shared_mutex`, perhaps `mutable`), the lock guards every other member that is not itself shareable or `const` and frozen. A safe function uses such a **locked member** of an object (`this`, or a variable named directly; a pointer only while it points where the guard was taken) only while a guard on that object's lock lives: a local `std::lock_guard`, `std::scoped_lock`, `std::unique_lock` or `std::shared_lock` constructed from the lock, until it is destroyed or unlocked. Under a `std::shared_lock` the members are only read. Locks are taken by guards, never by hand (`mu_.lock()`), and a borrow of a locked member ends with its guard ([`stale-borrow`](#stale-borrow), [`escape`](#escape)).

A lambda written as the argument of a call that does not keep it (a condition variable's predicate, an algorithm's callable) holds the guards held where it is written. A member function that expects the lock held takes the guard by reference, `void compact(std::unique_lock<std::mutex> &lock)`, and is called with a guard on the lock of the object it is called on. Constructors and destructors use the members without a guard, unless the class owns a thread that may already run or they have started one; a member that holds a thread is used under the lock outside them. A class with two locks is not a locked class: give each lock and the members it guards a class of their own. A ten-line `Locked<T>` with a `with(f)` member is Rust's `Mutex<T>`.

```cpp rejected
class Queue {
  std::mutex mu_;
  std::vector<std::string> items_;
public:
  std::size_t drain() {
    const std::size_t n = items_.size();
    items_.clear();
    return n;
  }
};
```

```cpp rejected
class Counter {
  std::mutex mu_;
  int value_ = 0;
public:
  void add() {
    mu_.lock();
    ++value_;
    mu_.unlock();
  }
};
```

```cpp accepted
class Queue {
  std::mutex mu_;
  std::condition_variable ready_;
  std::vector<std::string> items_;
public:
  void push(std::string item) {
    {
      const std::lock_guard lock(mu_);
      items_.push_back(std::move(item));
    }
    ready_.notify_one();
  }
  std::string pop() {
    std::unique_lock lock(mu_);
    ready_.wait(lock, [this] { return !items_.empty(); });
    std::string item = std::move(items_.back());
    items_.pop_back();
    return item;
  }
  std::size_t drain() {
    const std::lock_guard lock(mu_);
    const std::size_t n = items_.size();
    items_.clear();
    return n;
  }
};
template <class T> class Locked {
  mutable std::mutex mu_;
  T value_;
public:
  explicit Locked(T value) : value_(std::move(value)) {}
  template <class F> auto with(F f) {
    const std::lock_guard lock(mu_);
    return f(value_);
  }
};
```

## defined

What no rule rejects in a safe function has one result, or stops the program with a report, in the fast and the checked build alike ([RFC 0051](/rfcs/0051-nothing-undefined/)): signed arithmetic wraps, a shift's count is taken modulo the width, a conversion from floating point to an integer saturates (NaN to 0), a loop may run forever, every local starts as zeroes, and an object assigned to one it overlaps is copied as it was; a division by zero stops (`division-by-zero`), and so does the least value divided by -1 (`division-overflow`); reaching code the program says cannot be reached (`std::unreachable()`, `__builtin_unreachable()`) stops (`unreachable`), and so does the end of a function that must return a value (`missing-return`); a frame larger than a page is probed, so that a full stack stops (`stack-exhaustion`). The flags that would let the optimizer assume otherwise (`-ffast-math`, `-Ofast`, `-fstrict-enums`, `-fno-wrapv` and the rest) are refused with `-fweavec-safe`.

Five of Clang's warnings are errors in a unit compiled with `-fweavec-safe`, with Clang's own words: `-Wreturn-type` (a path that returns no value), `-Winvalid-noreturn` (a `[[noreturn]]` function that returns), `-Wformat` and `-Wformat-security` (a format that does not match its arguments, or is not a literal), and `-Wunsequenced` (an object changed twice, or read and changed, with nothing to order them); in C also `-Wdeprecated-non-prototype` (a call through a declaration without a prototype). They end in `[-Werror,-W<name>]`, not in a rule's identifier.

```text
sign.c:5:1: error: non-void function does not return a value in all control paths [-Werror,-Wreturn-type]
```

`weavec-cc --weavec-explain undefined` prints the list of the two languages' undefined behavior, each item with what disposes of it in a safe function and the test that shows it.

## definition-mismatch

The safe units of a program agree on what they share ([RFC 0051](/rfcs/0051-nothing-undefined/) §5): each records the layout and the WeaveC annotations of every type other units can name (a C++ class outside an unnamed namespace; in C, a struct a shared declaration names), the declaration of every function it defines or calls, and the type of every global it defines or names, and `weavec-cc` compares the records of a link's objects and the members of its archives before the linker runs (a linked image keeps no record: a shared library's units are compared when it is linked). Two that disagree are an error, and the program is not linked:

```text
weavec-cc: error: 'Entry' is defined differently in two safe units [weavec-safe:definition-mismatch]
cache.cpp:7:8: note: here it is 8 bytes: 'long hits' (cache.o)
index.cpp:8:8: note: here it is 56 bytes: 'std::array<long, 6> slots', 'long hits' (index.o)
weavec-cc: note: a program has one definition of each name: put a type only one file uses in an unnamed namespace, or define it in one header both include
```

Put a type only one file uses in an unnamed namespace (C: give it a name of its own), and compile a header every unit includes with the same macros. `-fno-weavec-definitions` links without comparing, and says so; it is for bisecting a build.
