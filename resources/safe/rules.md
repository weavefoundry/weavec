# The rules of Safe C and Safe C++

A safe function (`WEAVEC_SAFE`, or every function of a unit compiled with `-fweavec-safe`) is checked against these rules when it compiles. Each error ends with the rule's identifier, `[weavec-safe:<rule>]`, and `weavec-cc --weavec-explain <rule>` (or `weavec-c++`) prints the rule's section of this page; `--weavec-explain all` prints all of it. Every rule is decided inside one function, from the declarations it can see. A rule rejects a construct and says what to write instead: none tries to prove that code it does not accept is correct. What cannot be written in a safe function goes in a small `WEAVEC_GUARDED` helper, which `weavec-cc --weavec-audit` lists.

A rule that both languages have says first what it means in C, then, under _In C++_, what it means there. [`safe-cxx`](#safe-cxx) is the page to read before writing safe C++: the kinds of pointer its rules are stated over.

The C examples below include `<weavec.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>` and `<string.h>`; the C++ ones `<weavec.h>` and the standard library's containers, strings, smart pointers and algorithms, as C++20. The design is in [RFC 0041](/rfcs/0041-safe-c/) (bounds), [RFC 0042](/rfcs/0042-typed-memory/) (typed memory), [RFC 0043](/rfcs/0043-ownership/) (ownership) and [RFC 0044](/rfcs/0044-safe-cxx/) (C++).

## safe-cxx

Not a rule: what the rules of safe C++ are about. `weavec-c++ -fweavec-safe` checks every function the unit defines in its own files (not the standard library's or anything else behind `-isystem`), each lambda, and each instantiation of its templates. A safe C++ function is compiled exactly as without the flag: every guard and every check of the hardened standard library stays, so what the rules do not see still stops when it runs.

Every pointer, reference and object in a safe function is one of four kinds:

- An **owner** is an object: a local, a member, an element, and what a `std::vector`, a `std::string`, a `std::unique_ptr` or a `std::optional` holds. Destructors release, so a safe function writes no `new` and no `delete` ([`raw-owner`](#raw-owner)).
- A **borrow** is a reference, a pointer, an iterator, a `std::string_view`, a `std::span` or a lambda that captures by reference, kept in a local or passed down a call. It ends when what it points into is destroyed, moved, assigned, or changed by anything that is not `const`; using it afterwards is [`stale-borrow`](#stale-borrow). A member function that has a `const` overload with the same parameters (`operator[]`, `at`, `begin`, `find`) only gives access and ends nothing.
- A **view** is an object whose type holds a borrow: a struct with a `std::string_view` or a reference member, a `std::vector<std::string_view>`, a `std::function` made from a lambda that captures by reference. The rules follow it like a borrow: it is not used after what it points into ended, and it is not returned or stored where it would outlive that ([`escape`](#escape)).
- A **reference** is a raw pointer kept in a member or in a container's element. The rules do not follow it; the guards check it when it is used. What an object owns is not changed through one ([`owner-path`](#owner-path)); `std::shared_ptr` is the way to share an object that changes.

The other flow rules are [`owner-moved`](#owner-moved) (an object used after `std::move`) and [`call-alias`](#call-alias). The rules of single expressions are [`index-single`](#index-single), [`pointer-cast`](#pointer-cast), [`pointer-bytes`](#pointer-bytes), [`union-pointer`](#union-pointer), [`int-to-pointer`](#int-to-pointer), [`variadic`](#variadic), [`no-asm`](#no-asm), [`unsafe-block`](#unsafe-block), [`bad-annotation`](#bad-annotation) and [`not-modeled`](#not-modeled). Rules without an _In C++_ part are C's.

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

### In C++

A raw pointer points to one object: it is not indexed and takes no arithmetic (the difference and the comparison of two pointers are allowed). A pointer with a count is a `std::span`, whose index the library checks. An array is indexed in place and converts only to a `std::span`; a span is made from a container or an array, not from a pointer and a size. `main` may index and wrap its `argv`.

```cpp rejected
int third(const int *p) { return p[2]; }
```

```cpp accepted
int third(std::span<const int> s) { return s[2]; }
int local() {
  int a[4] = {1, 2, 3, 4};
  std::span<int> whole(a);
  return a[1] + whole[2];
}
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

A pointer converts to another pointer type only when no pointer can be forged: memory that holds pointers converts to nothing but `void` and `const` bytes; a `void *` converts to no other type, but where an allocation made it (`struct node *n = malloc(sizeof *n)`); function pointers do not convert.

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

### In C++

No `reinterpret_cast`, no `const_cast`, no C-style cast that is one of them, no `static_cast` from `void *` or from a base to a derived class, no `std::launder`. `dynamic_cast` and `std::variant` are the checked forms.

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

### In C++

The bytes of an object that is not trivially copyable are not read or written by `memcpy`, `memset`, `memmove` or `memcmp`.

```cpp rejected
struct Item { int id = 0; std::string name; };
void copy(Item &to, const Item &from) { std::memcpy(&to, &from, sizeof(Item)); }
```

```cpp accepted
struct Item { int id = 0; std::string name; };
void copy(Item &to, const Item &from) { to = from; }
```

## union-pointer

A union member that holds a pointer is not read: the union may hold something else.

```c rejected
union value { long number; char *text; };
char first(union value *v) { return v->text[0]; }
```

Keep a tag beside a struct per variant, or one pointer per variant in a struct.

### In C++

A union with a member that is a pointer, a view or an object with a destructor is not read in a safe function; `std::variant` keeps which alternative is live and checks it.

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

### In C++

A borrow or a view of a local is not returned, and is not stored where it outlives the function's locals: in a member of `*this`, through a parameter, in a global. That covers a reference or a `std::string_view` of a local, a lambda that captures a local by reference, and a `std::vector` of views into a local buffer. A view member is assigned only from parameters that are themselves views (a `std::string_view`, a `std::span`, a pointer), which callers can see, or bound in the constructor; it is not made to point into another member of the same object, which would break when the object is moved. A `std::function`, a `std::thread` or a `std::any` made from a view is a view. A function that keeps a callable it was passed (in a member, a container, a global) declares the parameter `WEAVEC_ESCAPES`, written after the type: `std::function<void()> WEAVEC_ESCAPES handler`. The argument for a `WEAVEC_ESCAPES` parameter is not the address of a local, nor a callable that captures one by reference.

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
  void subscribe(std::function<void()> handler) { handlers.push_back(std::move(handler)); }
};
```

```cpp rejected
struct Events {
  std::vector<std::function<void()>> handlers;
  void subscribe(std::function<void()> WEAVEC_ESCAPES handler) { handlers.push_back(std::move(handler)); }
};
void count_events(Events &events) {
  int count = 0;
  events.subscribe([&count] { ++count; });
}
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
struct Events {
  std::vector<std::function<void()>> handlers;
  void subscribe(std::function<void()> WEAVEC_ESCAPES handler) { handlers.push_back(std::move(handler)); }
};
void count_events(Events &events, std::shared_ptr<int> count) {
  events.subscribe([count] { ++*count; });   // captures by value
}
struct Document {
  std::string buffer;
  std::size_t title_length = 0;              // an offset and a length, not a view
  std::string_view title() const { return std::string_view(buffer).substr(0, title_length); }
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

### In C++

Safe C's bounds annotations and `WEAVEC_OWNED` are not used by a safe C++ function: a pointer with a count is a `std::span`, and an owner is a `std::unique_ptr` or a container. `WEAVEC_ESCAPES`, `WEAVEC_BORROWED` and `WEAVEC_LIFETIMEBOUND` keep their meaning.

```cpp rejected
int first(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n) { return n != 0 ? *p : 0; }
```

```cpp accepted
int first(std::span<const int> s) { return s.empty() ? 0 : s[0]; }
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

## string-write

Nothing is stored through a null-terminated pointer, nor passed where the callee may write through it: a string is read-only in safe code. A buffer that is written is counted.

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

### In C++

What an object owns is changed only from what owns it or was lent it: a local, a parameter, `this`, and through members, owners (`std::unique_ptr`, containers) and borrows. Through a raw pointer kept in a member or in a container's element, a safe function reads, calls `const` member functions and assigns members that own nothing; it does not call a member function that is not `const` on an object that owns something, or assign such a member. That is what lets a borrow survive a call that was not handed its owner: no callee can reach the owner through a pointer it kept. The fixes: own the object; take it as a parameter; make the member a reference, which makes the class a view that lives in a local; or hold it by `std::shared_ptr`, through which changes are allowed. A member function that is `const`, or has a `const` twin, changes nothing `*this` owns, `mutable` members included.

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
struct Node {
  std::vector<std::unique_ptr<Node>> children;
  Node *parent = nullptr;                           // read, never changed through
  int depth() const { return parent != nullptr ? parent->depth() + 1 : 0; }
  void add(std::unique_ptr<Node> child) {
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
- was changed: a member function that is not `const` and has no `const` twin was called on it (`push_back`, `erase`, `clear`, `reset`, `operator[]` of a map, which inserts), or it was passed to a function through a reference or pointer that is not `const`.

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

In one call of a function of the program, an argument through which the callee could change what an object owns (a reference or pointer that is not `const`, the object of a member function that is not `const`) is not passed beside a borrow into the same object. The standard library's own functions are trusted with such arguments (`v.push_back(v[0])`, `std::sort(v.begin(), v.end())`); what is rejected there is a callable that changes an object it captured by reference, passed beside a borrow into that object.

```cpp rejected
void append(std::vector<std::string> &to, const std::string &s);
void repeat_first(std::vector<std::string> &v) { append(v, v[0]); }
```

```cpp rejected
void duplicate(std::vector<int> &v) {
  std::for_each(v.begin(), v.end(), [&](int x) { v.push_back(x); });
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
```

## raw-owner

Safe C++ only. A block is owned by an object, never by a raw pointer: no `new`, no `delete`, no explicit destructor call, no `malloc`, `free` or `strdup`, no `release()` of a smart pointer, no smart pointer made from a raw pointer. `std::make_unique`, `std::make_shared` and the containers make blocks with their owners, and destructors release them, so a double release, a mismatched `delete` and a forgotten release cannot be written. A class that must manage memory itself (a container, an arena, a wrapper of a C library's handle) is `WEAVEC_GUARDED`: it is the implementation of an owner, and safe code uses it as it uses the standard library's.

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

Safe C++ only. A construct the rules do not follow is rejected, not guessed at: a coroutine, `goto`, a GNU statement expression, a function that returns twice (`setjmp`). Write it another way, or keep the function `WEAVEC_GUARDED`.

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
```
