# RFC 0053: Callables and containers by their loans

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-21
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes, for Safe C++,
  [RFC 0052](0052-what-a-call-may-reach.md) §5.1 (what a callable the
  function sees may change), §5.3's reading of a kept callable by its
  captures, D5's "a lambda the function can see", and §6's reading of a
  value built from borrows by the call that builds it. Answers RFC 0052's
  _Unresolved question_ 4 (virtual calls). Adds nothing to Safe C, the
  guards or the runtime.

## Summary

RFC 0052 made Safe C++'s borrows end by default at a call that could reach
them, decided from types. Its gate G4, a fresh adversarial round with the
compiler that implements it, found 71 programs of its own classes that
compile as safe C++ with no error and then write a string into a live
`long` or read another object's bytes. Two thirds share one cause: the
rules follow a callable only when they can find the lambda that made it by
the form of an expression (`lambdaOf`), and read what it changes from that
lambda's body. Every other way to hold a callable is a form the rules do
not name:

- `std::bind`, `std::bind_front`, `std::mem_fn`, a pointer to a member
  function and the object it is called on;
- a lambda in a `std::pair`, `std::optional`, `std::any`, `std::variant`,
  `std::array` or `std::vector`, behind a pointer, through `?:` or an
  identity function, or returned by a factory;
- a functor class of the program;
- a lambda whose body changes a capture through a local reference or
  pointer of its own, or whose init capture holds a view.

The rest is §6 asked of the library: `std::vector<std::reference_wrapper<V>>
{v}`, `std::pair<std::string *, int>` and `std::optional<long *>` are not
views of what they were made from; and of things RFC 0052 left aside: a
view behind a base class, a `thread_local` global, a `mutable` member, a
destructor declared `= default`.

This is the shape of the list RFC 0052 set out to replace, one level down.
This RFC removes the list for callables and containers the way RFC 0052
removed it for calls:

- **A callable is a view, and its call reaches what it holds (D1).** Any
  object of a callable type (a lambda, the library's erased callables and
  binders, a class of the program with an `operator()`, a pointer to a
  member function with its object) has loans like any view. A call of it,
  or a call it is passed to, ends the borrows into what its loans reach.
  Reading a lambda's body only narrows that: a capture every use of which
  is provably a read ends nothing.
- **The library's containers of views are views (D4).** A container,
  optional, pair, tuple, array, variant or smart pointer whose elements are
  views or pointers is a view of what it is made from, however it is made.
- **What a call cannot see is assumed to reach** what it could: a virtual
  function that is not final reaches what its object's loans reach (D5); a
  call of a function of the program reaches every mutable global (D6);
  `mutable` makes `const` shallow (D7).

Nothing changes at run time.

## Motivation

### The round (RFC 0052's G4)

Two agents, briefed as RFC 0052's round was and told what it closes, wrote
225 probes against the compiler of RFC 0052 (`build/rfc52/tc`). Every hole
below was rerun (`build/rfc52/g4/lifetimes/run-holes.sh`,
`build/rfc52/g4/types/run-holes.sh`): each compiles under `-fweavec-safe`
with no error, every function safe, and prints an impossible value in the
fast and the checked build with no `weavec:` report.

| Class | Holes | Examples |
| --- | ---: | --- |
| A callable the rules do not find | 27 | `std::bind(set, std::ref(v))`; `pr.first()` of `std::pair<L, int>`; `(*fp)()`; `(m.*pm)()`; `auto f = mk(v); f();`; `std::visit(Vis{v}, w)` |
| A callable passed beside what it changes (`call-alias`) | 7 | `run(v, mk(v))`; `run(v, [r = std::ref(v)] { … })`; `run(v, std::bind_front(&M::flip, m))` |
| A kept callable that is not a lambda (D6) | 6 | `b.f = std::bind(set, std::ref(v))`; `cb = std::bind_front(&C::flip, this)`; a functor holding `V &` kept in a `std::function` |
| A change the body makes through its own alias | 3 | `[&v] { V &r = v; r = K; }` |
| A binder of a local's address, returned or kept | 3 | `return std::bind_front(&L::set, &l);` |
| A library container of views, made from braces | 7 | `std::vector<std::reference_wrapper<V>> rs{v}; rs[0].get() = K;` |
| A value of the library holding a pointer, returned | 3 | `std::pair<std::string *, int> g(V &)` |
| A view's result, through a member or a callable | 4 | `struct P { V *p; std::string &s() const; }`; `std::function<std::string &()>` |
| `lifetimebound` on the wrong parameter, not checked | 1 | `pick(V &a [[clang::lifetimebound]], V &b) { return get(b); }` |
| A view behind a base, or a polymorphic owner | 3 | `go(Base &b) { b.run(); }` with `D : Base { V &v; }`; `std::unique_ptr<Base>` |
| A global or a `thread_local` | 4 | `thread_local V g`; `flip() { g = K; }`; `V &get() { thread_local V v; return v; }` |
| `mutable` | 2 | `struct Box { mutable V v; }; f(const Box &b) { b.v = K; }` |
| `~T() = default` taken for an owner | 1 | `struct Own { long *p; ~Own() = default; }; return Own{&x};` |

The lifetimes agent also found 19 programs the rules accept that the
checked build stops at run time (`vector<long *>{&x}` returned, an
aliasing `shared_ptr` stored past its local). RFC 0052's _Soundness_
claims them; D4 rejects them at compile time.

One more hole is not of these classes: `std::as_writable_bytes` of a span
of `std::string` writes bytes over a string's pointer, which RFC 0052's _No
object in bytes_ assumes `pointer-bytes` rejects. It belongs with RFC 0051's
and PR 78's rules over bytes and is not this RFC's.

### Why patching is the wrong fix, again

RFC 0052 found the lambda a call runs by following an expression back:
through casts, temporaries, a one-argument construction, a local's
initializer. Each hole above is a step that walk does not take. Adding
`std::pair::first`, `std::bind`'s result type, `?:`, `std::any_cast` and
the rest would be a list of the ways to hold a callable, and the next round
would find the next one, as RFC 0052's round found the 25th call.

The flow already has the general answer. A callable that captured `v` by
reference is a view of `v`: RFC 0044 made lambdas view classes, and the
flow follows their loans through every copy, member, element, pointer and
result as it follows a `std::string_view`'s. What RFC 0052 read from the
lambda's body (which captures it changes) is a narrowing of that answer,
not the answer. Taken the other way round, a callable the function cannot
read is no longer a callable the rules miss: it is a view whose call
reaches what it points to, as RFC 0052 §4.1 already says of a view of the
program's class.

### What it costs, and what S0 measures first

The default is stricter than RFC 0052's where the body cannot be read, and
where it can, a capture the body only reads must be proven so. RFC 0052's
prototype measured the strictest form once: when every call of a callable
the function could not see ended every borrow of what is not the
function's own local, it gave 16 errors in 157 correct files (4 of them
the design, loops over stored callbacks). This RFC's default is narrower
than that (what the callable's loans reach, not everything foreign; lent
and kept callables stay as RFC 0052 has them), and RFC 0052's G2 left
little room: 1.84 new errors per thousand safe functions against D8's 2.
S0 measures both forms of D1 (with and without the narrowing) on the same
corpus before anything else is built, and returns to the owner if D8's
limit is exceeded.

### Measured: S0 (`build/rfc53/s0/`)

A prototype of §1 to §8 on RFC 0052's branch (`prototype.diff`, 435 lines
in `SafeCxxFlow.cpp` and `SafeCxxKinds.h`), each decision behind a switch,
measured against RFC 0052's compiler (`build/rfc52/tc`) on RFC 0052's G2
corpus (`measure.py`: 10,413 safe functions in the RFC 0044 and RFC 0046
files, interp and kv made safe, tinyxml2, yaml-cpp, the json port, libzmq's
and nng's conversions) and on the 71 holes and 19 programs the checked
build stops (`holes.sh`, 90 programs).

| Decisions | New errors | Per thousand | Programs rejected (of 90) |
| --- | ---: | ---: | ---: |
| RFC 0052 | — | — | 0 |
| D1 (narrowed) and D2 | 0 | 0.00 | 62 |
| D1 without the narrowing, and D2 | 16 | 1.54 | 62 |
| D1 to D7, as drafted | 247 | 23.72 | 72 |
| … without D4 | 155 | 14.89 | 70 |
| … without D5 | 161 | 15.46 | 71 |
| … without D7 | 102 | 9.80 | 70 |
| … without D3, or without D6 | 247 | 23.72 | 70, 69 |
| D1, D2, D3, D6, D7 read at the caller | 2 | 0.19 | 69 |
| … and D4 | 18 | 1.73 | 71 |
| … and D5 as §6 now has it | 18 | 1.73 | 72 |

So:

- **The narrowing (Unresolved question 1) buys 16 errors and no hole.**
  Without it, every lambda that captures `this` is taken to change all of
  `*this`, and RFC 0046's thread pools pass one beside a member (16
  `call-alias`); with it, a body that names members changes those members,
  as RFC 0052 amendment 4 reads it. Keep it.
- **D1, D2, D3 and D6 cost nothing** on this corpus and reject 67 of the
  90 programs between them. A callable read by its loans is the default
  the code was already written to: none of the corpus's lambdas, binders or
  `std::function`s holds a borrow across a call of it that the call could
  change.
- **D7 as drafted cost 145 errors**, all `owner-path` in tinyxml2's and
  yaml-cpp's own `const` member functions over their `mutable` caches,
  because the prototype read `mutable` into what a `const` member function
  of the class itself may do. Read only at the caller (a `const` call on an
  object with `mutable` owners ends the caller's borrows into it, and asks
  nothing of the class), it costs 2 (tinyxml2, `owner-path` in a derived
  class calling its base's `const` function) and closes the 2 `mutable`
  holes. §7 is amended to say so.
- **D4 costs 16** for 2 more programs: 5 `escape`s of a local's address
  kept in a container of pointers (libzmq's timer map, whose entries hold
  a `void *`, which reaches everything), and borrows of kv's `Executor`
  through a container of views. It fits D8 with D1 to D3, D6 and D7
  (1.73), with little room.
- **D5 as drafted cost 86 for one program.** Taking every polymorphic
  class to hold a borrow made every `std::unique_ptr<Expr>` a parser
  returns a view of the parser (interp's 80), and every virtual call a
  barrier (RFC 0044's shapes). §6 now has the narrower form: a polymorphic
  object holds the loans of what it was made from and none by default.
  Its first prototype still cost 52 (a member function's result taken to
  borrow its object whatever its type, a polymorphic argument taken to
  reach what its container points to, a socket's member calls taken to
  store their pointers); with those read as §6 says, it costs nothing
  (18 with every decision, as without D5) and rejects one more program, a
  view behind a base passed to a function. It does not reject a view
  returned as a `std::unique_ptr<Base>` by a function, which the caller
  cannot see (_Unresolved question_ 3).

The 18 programs every decision, D5 as §6 has it, still accepts are 2 of
D5's (a `unique_ptr<Base>` and a `unique_ptr` of a view, returned by a
function), 13
of D4's form the prototype does not reach yet (a `std::array` or
`std::optional` of reference wrappers made from braces, `vector<long *>{&x}`
returned, a pointer member assigned past its local's scope, an aliasing
`shared_ptr` stored, an out parameter of `optional<reference_wrapper>`,
`pair`, `vector` and `optional` of pointers returned), a lambda in a
`std::array`, a lambda reached through a pointer passed beside what it
changes, and a function's `static` returned by reference (D6).

RFC 0052's own G2 left 20 new errors against main; with D1 to D4, D6 and
D7 the two RFCs together give 38 in 10,413 functions against main (3.6
per thousand), which D8 as written does not limit, since each RFC is
measured against the one before.

## Soundness

### What is guaranteed that was not

(X3) _No stale borrow_ holds for every callable a safe C++ function holds,
not only for lambdas it wrote: a safe function does not use a borrow after
a call of a callable value, or a call that value is passed to, whose loans
reach the borrow's owner, unless every use the callable can make of that
owner is a read. It does not keep a callable whose loans reach anything but
owners, and does not pass one beside what its loans reach.

A container, optional, pair, tuple, array, variant or smart pointer of the
library that holds views or pointers is a view of what it was made from,
and is not returned or kept past it.

A borrow into a mutable global ends at every call of a function of the
program; a borrow reached through an object of a polymorphic class ends at
a call of a virtual function of it that is not final.

### What it rests on

RFC 0052's assumptions unchanged, with X-A2 read for every callable: a
callable code that is not safe gives a safe function (lent, as RFC 0052
§5.2 has it) does not change what the function's other parameters reach.
A safe caller is checked for it, now for every callable type.

### Bugs caught

The 71 holes above but `as_writable_bytes`, as `test/Safe/cxx-loans-*.cpp`,
and the 19 programs the checked build stops. Among them:

```cpp
using V = std::variant<std::string, long>;
void set(V &v) { v = 1L; }
void bound(V &v) {
  auto f = std::bind(set, std::ref(v));    // a view of 'v'
  std::string &s = std::get<std::string>(v);
  f();
  s[0] = 'Z'; // stale-borrow: 's' was taken from 'v' before the call of 'f',
              // which may change what 'f' holds, 'v'
}
```

```cpp
void paired(V &v) {
  auto g = [&v] { v = 1L; };
  std::pair<decltype(g), int> p{g, 1};      // a view of 'v'
  std::string &s = std::get<std::string>(v);
  p.first();
  s[0] = 'Z'; // stale-borrow
}
```

```cpp
void wrapped(V &v) {
  std::vector<std::reference_wrapper<V>> refs{v};   // a view of 'v'
  std::string &s = std::get<std::string>(v);
  refs[0].get() = 1L;
  s[0] = 'Z'; // stale-borrow
}
```

```cpp
struct Base { virtual void run() = 0; virtual ~Base() = default; };
struct D : Base { V &v; explicit D(V &v) : v(v) {} void run() override { v = 1L; } };
void go(Base &b) { b.run(); }
void through(V &v) {
  D d(v);                                   // a view of 'v'
  std::string &s = std::get<std::string>(v);
  go(d);       // reaches what 'd' holds, through the 'Base &' it is bound to
  s[0] = 'Z';  // stale-borrow
}
```

### Bugs deliberately not caught

- **Threads**, as RFC 0052 left them: what a thread start gives a thread is
  the next RFC's.
- **Shared ownership** (RFC 0044 §8): a change through another holder of a
  `std::shared_ptr`, including one a kept callable captured.
- **Bytes** (`std::as_writable_bytes`), which is RFC 0051's and PR 78's.
- **Code that is not safe**, trusted with X-A1 and X-A2 as today.

### Accepted rejections

- **A local callable that only reads what it captured, where the reading
  cannot be proven.** `auto print = [&v] { log(v); };` passes `v` to a
  function of the program by `const &`; a read. `auto size = [&] { return
  helper(v); };` where `helper` takes `V &` is not, and ends borrows into
  `v` at each call of `size`. The fix is a `const &` capture
  (`[&v = std::as_const(v)]`) or a `const` parameter.
- **A virtual call on a polymorphic object made from a borrow.** The
  borrows into what it was made from end at the call, whatever the
  override does. The fix is to take the borrow after the call, or call
  through a `final` class.
- **A borrow into a mutable global across a call.** The fix is to copy out
  what is needed, or make the global `const` or a locked class.

## Detailed design

Everything is in `lib/Frontend/SafeCxxFlow.cpp` and `SafeCxxKinds.h`. No
rule is added; `stale-borrow`, `call-alias` and `escape` gain cases.

### 1. A callable's loans (D1)

A **callable type** is a lambda's class, a library class the table marks
`erased` with an `operator()`, a class (of the program or the library: a
binder) with an `operator()`, or a pointer to a member function together
with the object it is called on. An object of one is a view: its loans are
what its members point to (RFC 0044's view classes, RFC 0052 §6's
borrowing classes, read structurally for the library's binders), wherever
it is held, in a local, a member of a local, an element, a temporary, a
call's result, or behind a pointer.

- **Its call.** A call of `operator()` on it (directly, through a pointer,
  as `std::invoke`, through `.*` or `->*`) ends the borrows `Into` what its
  loans reach, with `Ends::Into` at the depth of each loan, as RFC 0052 §4.1
  does for a view of the program's class.
- **A call it is passed to.** A call given it (any parameter, any callee,
  the library's included) ends the same borrows, unless the parameter keeps
  it (§3).
- **Narrowing by a body that can be read.** When the callable's value is a
  lambda the function wrote (RFC 0052's `lambdaOf`, every instance of a
  generic one), a capture by reference every use of which in every
  instance's body is a read ends nothing at the call. A read is what RFC
  0046's `constUse` already decides for a loan to a thread: read, copied,
  passed by value or by reference to `const`, given a `const` member
  function of an owner. A local of the body that aliases a capture (a
  reference or pointer initialized from it) is the capture.
- **Its result.** What a call of a callable returns points into what its
  loans reach and into its arguments, by RFC 0052 §3.

S0 measures D1 with the narrowing and without it.

### 2. `call-alias` for callables by their loans (D2)

RFC 0052 §5.2 compares what a lambda's body changes with what is passed
beside it. This compares what any callable's loans reach (narrowed as in
§1) with the other arguments and the object, for every callee.

### 3. Kept callables by their loans (D2)

RFC 0052 §5.3's rule, read from loans: a callable of any type that is kept
holds no loan but of owners (a `std::shared_ptr`, a `std::weak_ptr`). So
`b.f = std::bind(set, std::ref(v))`, `cb = std::bind_front(&C::flip, this)`
and a functor holding a reference, stored in a `std::function` member, are
`escape`. RFC 0052's amendment 6 stands: a callable a local of the function
keeps is one the function sees.

### 4. A call given a callable is the program's (D3)

RFC 0052 amendment 3 trusts the library's functions with what a view they
are given points to. A library function given a callable calls it with the
other arguments (`std::invoke(&M::flip, m)`, `std::visit(Vis{v}, w)`,
`std::mem_fn(f)(m)`, an algorithm): for §4.1 such a call is a function of
the program's, and reaches what its view arguments point to.

### 5. The library's containers of views (D4)

A library class that holds views or pointers of the program's (by a
template argument that is a view or borrowing class, a pointer or a
reference wrapper; by structure, through the blocks an owner keeps) is
borrowing (RFC 0052 §6): tracked by the flow, a view of each argument it is
made from that its elements could point to or into, from braces (an
`initializer_list` constructor, an aggregate `std::array`), from a
converting construction or assignment, as a result and through an out
parameter. A member function of it that gives access to an element
(`operator[]`, `front`, `get`, `*`, `->`) gives a borrow at where the
container's loans point, as well as into the container.

### 6. Polymorphic objects (D5)

A polymorphic class of the program that is not `final`, and a library
template of one (a smart pointer to a base), is followed by the flow, with
the loans of what it was made from and **none by default**:

- A conversion keeps the loans of what it converts: a `D &` bound as a
  `Base &`, a `std::unique_ptr<D>` converted to a `std::unique_ptr<Base>`.
- A call of a virtual member function that is not `final` on such an
  object, or a call of a function of the program it is passed to, reaches
  what the flow knows its loans reach, as a view of the program's class
  does under RFC 0052 §4.1; nothing more, so an `Expr &` taken from a
  container reaches nothing the container's own loans do not.
- A member function's result of such a type borrows its object only if
  the result's type could (RFC 0052 §6's `mayBorrow`), and a call of its
  member functions stores nothing of its arguments (it is not a view).

So a view behind a base the function made is followed through every
reference and smart pointer to it. One a function returns as a
`std::unique_ptr<Base>` is not: the caller cannot see the derived class,
and taking every such result to borrow its arguments cost 80 errors in S0
(_Unresolved question_ 3). This answers RFC 0052's _Unresolved question_ 4
for what the function can see.

### 7. Globals, `mutable`, owners (D6, D7)

- A borrow whose root is a global that is not `const` (a namespace-scope
  variable, a `static` member, a `thread_local`, a function's `static`), or
  that a function of the program returned with no argument it could point
  into, ends at every call of a function of the program and at every
  change through another such result.
- A `const` call on an object of a class with a `mutable` member that
  holds owners (a `const` member function, or passing it by `const`
  reference) ends the caller's borrows into the object as a call that is
  not `const` does. It asks nothing of the class's own `const` member
  functions (`owner-path` stands as RFC 0044 has it), which S0 measured
  to cost 145 errors over tinyxml2's and yaml-cpp's caches.
- A class of the program is an owner of what its pointers point to only if
  its destructor is user-provided; one declared `= default` is not.

### 8. Fixes to RFC 0052's implementation

These bring the code in line with RFC 0052 and need no decision:

- §3's "a safe function is checked against the same reading": a safe
  function that returns a borrow of a parameter without
  `WEAVEC_LIFETIMEBOUND` when another parameter has it is `escape`.
- A member function of a borrowing class returns where its object points,
  as a view class's does (`P::s()` above).

### 9. What does not change

Run time, Safe C, the ledger and its version, the annotations, RFC 0052's
§2 to §4 and §6 for values the program builds.

### 10. Fixes the errors carry

| Error | Fix | Certainty |
| --- | --- | --- |
| `stale-borrow` at a call of a local callable that only reads a capture the rules cannot prove it reads | capture it by `const &` (`[&v = std::as_const(v)]`) | likely (E9: no use in the body changes it, as `constUse` reads it) |
| `stale-borrow` at a virtual call | none written; the note names `final` | — |
| `escape`, a kept binder or functor holding a borrow | none written; the note names capture by value and `std::weak_ptr` | — |

### 11. Stages and gates

- **S0. Measure.** A prototype of §1 (with and without the narrowing), §3,
  §5 and §7 on RFC 0052's branch, against RFC 0052's G2 corpus (the 157
  files, `test/Safe`, RFC 0049's conversions, the json port, RFC 0051's
  trial programs), with `build/rfc52/s0/impl/measure.py`. Report the holes
  rejected, the new errors per thousand safe functions and which a fix
  covers; return to the owner if D8's limit is exceeded.
- **S1.** Callables by their loans, `call-alias` and kept callables (§1 to
  §4).
- **S2.** The library's containers of views (§5).
- **S3.** Polymorphic objects, globals, `mutable`, owners (§6, §7), and
  §8.
- **S4.** The fixes (§10), the rules' pages, the guide's "Borrows" and
  "Callbacks", the guarantees page ((X3), (X6)), the roadmap.

Gates:

- **G1. RFC 0052's round.** Its 71 holes of these classes and its 19
  programs the checked build stops are rejected, pinned by
  `test/Safe/cxx-loans-*.cpp`.
- **G2. Correct code.** As RFC 0052's G2, within D8's limit, each new error
  listed with its fix or the reason there is none.
- **G3. The loop.** As RFC 0052's G3: RFC 0051's trial programs made safe by
  a fresh agent in no more than 1.5 times RFC 0051's time.
- **G4. A fresh round** on the lifetimes and types classes finds no hole of
  RFC 0052's or this RFC's classes.
- **G5. Compile time** within 1.03 of RFC 0052's, with a Release compiler.
- **G6. Same code** as RFC 0052's, modulo the ledger and main's own
  variation between runs.

## Annotation surface

None.

## Diagnostics

No new rule. New messages under existing rules:

- `stale-borrow`: `'<b>' was taken from '<o>' before the call of
  '<callable>', which may change what '<callable>' holds, '<o>'`.
- `stale-borrow`: `'<b>' points into '<o>', which the virtual call to
  '<f>' may reach through '<object>'`.
- `stale-borrow`: `'<b>' points into the global '<g>', which the call to
  '<f>' may change`.
- `call-alias`: `'<callable>' holds a borrow of '<o>', which is passed with
  it`.
- `escape`: `'<callable>' is kept by '<keeper>' and holds a borrow of
  '<o>'`.

Each is pinned by a `test/Safe/` test with its note.

## Drawbacks

- **More correct code is rejected**, and how much is the open question S0
  answers. The narrowing keeps RFC 0052's precision for lambdas the function
  wrote whose captures are only read; it loses it where a capture is passed
  to a function of the program by non-`const` reference that only reads.
- **Virtual calls end the borrows** into what a polymorphic object was
  made from. S0 found none in the corpus; and a view returned behind a base
  by a factory is not followed (_Unresolved question_ 3).
- **Globals.** A borrow into a mutable global no longer survives a call of
  the program's. Safe code has few: RFC 0046 makes every global shareable,
  `const` or `thread_local`.

## Alternatives

- **Extend `lambdaOf`.** Follow pairs, optionals, binders, pointers and
  factories to the lambda. That is the list again; rejected for RFC 0052's
  reason.
- **Reject every callable but a lambda in safe code.** Simple and sound,
  and it rejects `std::function` members, binders and functors, which the
  guide recommends and the trial programs use.
- **Read the bodies of functors and factories in the unit.** It can be
  done (the same unit), and it makes a function's verdict depend on the
  bodies of others, which every rule of WeaveC avoids (RFC 0035 §6).

## Prior art

- **Rust.** A closure is a value whose type carries its captures' borrows;
  `Fn`, `FnMut` and `FnOnce` say whether calling it reads, changes or
  consumes them, and a `&mut` capture is a unique borrow for the closure's
  life. D1 is the same default with the borrow read from loans, and the
  narrowing stands in for `Fn` where the body shows it.
- **C++ Core Guidelines' lifetime profile** (P1179): a lambda's points-to
  set is its captures', and a call of it invalidates what non-`const`
  captures point to.

## Unresolved questions

1. **The narrowing.** Is `constUse` enough to prove a capture is only read,
   or should a lambda the function wrote be read as RFC 0052 reads it
   (what it changes) with the aliases of §1 added? S0 measures both.
2. **Function pointers.** A pointer to a function of the program holds no
   loans; its call reaches only its arguments and the globals (D6). Is
   that enough, or is a call through one a barrier for what its arguments
   reach?
3. **Interfaces returned by factories.** D5 follows a view behind a base
   only from where the function made it. A `std::unique_ptr<Base>` a
   function returns may hold a derived class's reference to an argument
   (RFC 0052's G4 `t09`); taking every such result to borrow its
   arguments cost 80 errors in S0. Should a function that returns one say
   what it borrows (`WEAVEC_LIFETIMEBOUND` on the parameter), with no
   borrow assumed otherwise, as X-A1 already assumes of code that is not
   safe?

## Future work

- **Threads**, as RFC 0052's _Future work_ has them.
- **Bytes**: `std::as_writable_bytes` and the spans of bytes over objects
  that hold pointers, with RFC 0051's and PR 78's rules.
- **A variant of trivially destructible alternatives** (RFC 0052
  amendment 12).

## Decisions to accept

Accepted on 2026-11-22 as S0 measured them: D1 with the narrowing
(_Unresolved question_ 1), D5 as §6 has it, D7 read at the caller.

- **D1. Callables by their loans:** a call of any callable value, or a call
  it is passed to, ends the borrows into what its loans reach; a lambda's
  body the function wrote narrows only by proof that a capture is read.
- **D2. `call-alias` and kept callables by loans**, for every callable
  type.
- **D3. A library function given a callable** is the program's for RFC
  0052 §4.1.
- **D4. The library's containers of views are views** of what they are made
  from, however they are made.
- **D5. A polymorphic object holds the loans of what it was made from**,
  none by default; a virtual call that is not final, or a call it is
  passed to, reaches those. (S0: the drafted form, every polymorphic class
  holding a borrow, cost 86 errors for one program; this one costs none.)
- **D6. A borrow into a mutable global** ends at every call of a function
  of the program.
- **D7. `mutable` makes `const` shallow for a caller; `= default` is no
  owner's destructor.**
- **D8. The cost limit** stays RFC 0052's: at most 2 new errors per
  thousand safe functions on G2's corpus, each with a fix or a stated
  reason, measured against RFC 0052's compiler. S0 returns to the owner if
  the design exceeds it.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what the gates found. They override the body.

1. **A callable that holds only views of `const` reaches nothing (§1).** A
   binder or functor whose borrowing members all name `const` objects that
   are not views, callables or classes with `mutable` owners
   (`std::bind(print, std::cref(v))`) only reads: its call, a call it is
   passed to and `call-alias` end nothing for it (`CxxKinds::readsOnly`).
   What its call returns still points where its loans reach.
2. **A callable's loans reach the callables they name (§1, §2).** A lambda
   behind a pointer another lambda captured (`[gp] { (*gp)(); }`) runs
   where the outer one runs: the reach of a call includes the loans of
   every callable local its own loans name (`reachOf`).
3. **D4, as built (§5).** An array made from braces (the elements of a
   `std::array`) points where its elements do. A block of pointers an owner
   keeps (`std::vector<T *>`'s) borrows what they point to, and a class
   made from a list copies its elements: `std::vector<long *>{&x}` borrows
   `x`, not the array the braces made. A member call through a pointer to
   a class that borrows and owns nothing (`o->get()` of an optional of a
   reference wrapper) points where the class does. A pointer read from a
   member or an element of a local of a library class the flow follows (a
   `std::pair`, an optional, a container of pointers) points where the
   local's loans do (`heldIn`). A pointer member of a class of the program
   stays a reference (RFC 0044 §8), tested where it is loaded: reading the
   loans of a local of such a class cost 7 errors in G2 (yaml-cpp's
   iterator values and simple keys, libzmq's endpoints) for no hole. Only
   the address of a local assigned to one is followed, so the struct does
   not outlive it (`Keep k; { long x; k.p = &x; }`).
4. **Out parameters (§5).** A safe function that stores a borrow of a
   reference parameter in a view that another parameter refers to (`o =
   std::ref(std::get<std::string>(v))` for an `optional<reference_wrapper>`
   out parameter) is `escape` unless the first parameter says
   `WEAVEC_ESCAPES`, as RFC 0048 §4 asks of every kept address. A caller
   then takes the out parameter's local to borrow what it passed for the
   kept parameter: at it, and into it unless the local's type points at
   it (RFC 0052 amendment 2). A raw pointer kept this way stays a
   reference.
5. **Assigning a shared pointer (§5).** A local assigned a shared pointer
   that the aliasing constructor made holds that pointer's borrow, and no
   longer the one it held.
6. **D6's results are at a global, and the callee is checked (§7).** A
   reference or pointer a function of the program returns with nothing it
   could point into is at "the globals", depth 0, which a call may assign
   but not destroy. Taking it to be inside one cost 7 errors in G2: a
   reference to a singleton (`Logger &l = Logger::instance(); l.set(…)`)
   was ended by its own member calls. In exchange, a safe function returns
   no reference or pointer into a global that is not `const` (`escape`:
   `std::string &name() { return std::get<std::string>(g); }`), which
   closes the `thread_local` result of the round.
7. **_Unresolved question_ 3: the callee says it (§6).** A safe function
   returns a value with a borrow of a parameter only if the value's type
   shows it as its callers read it (a member could name what the parameter
   reaches) or the parameter is `WEAVEC_LIFETIMEBOUND`; a shared pointer
   made from a shared pointer parameter shares its ownership. A caller
   reads a value a `lifetimebound` parameter gives as borrowing at the
   argument and into it, whatever the value's type. So a
   `std::unique_ptr<Base>` factory of a derived class that holds a
   reference is told to mark it, and its callers see the borrow
   (`t09`). This costs nothing on G2's corpus.
8. **_Unresolved question_ 2.** A pointer to a function holds no loans, as
   drafted; the round's `c19` (`call(&g)`) is rejected through the
   function the callable reaches. No probe of G4 needed more.
9. **A member of a local that is changed takes borrows only if it is
   followed (RFC 0052 §7).** `cfg.client_command.assign(args.begin(),
   args.end())` copies strings into a vector of owners: the local `cfg`
   does not borrow `args`. (RFC 0052 widened every such local; amendment 7
   above made it visible as one error in kv.)
10. **One report for a store.** A store a callee's parameter keeps
    (`registry.keep(&local)` of a class that borrows) is reported as kept
    by the callee, and not again as a store in its object.
11. **Messages (Diagnostics).** As built: a callable whose body the
    function did not write "may change what '<callable>' holds, '<o>'"; a
    lambda it wrote "changes '<o>'", as RFC 0052 has it. A virtual call is
    "the virtual call to '<f>' may reach through '<object>'", and its note
    names a `final` class. A call through a pointer to a member function
    is "the call through a pointer to a member function". A borrow into a
    global "points into the global '<g>', which the call to '<f>' may
    change". `call-alias` keeps RFC 0052's "'<callable>' may change '<o>',
    which is passed with it". New `escape`s: "what is returned holds a
    borrow of '<p>', which its type '<T>' does not show"; "what is returned
    points into the global '<g>', which a call of the program may change";
    "'<o>' would keep a borrow of the parameter '<p>', which is lent for
    the call only".
12. **§10's E9 is not built.** With the narrowing, a lambda whose every use
    of a capture is a read (`constUse`) already ends nothing, so each
    remaining error at its call has a use that is not a read (a function
    taking the capture by reference that is not `const`), where a `const &`
    capture would not compile. The note says what to do; no fix is
    written. The other two rows of §10 are notes, as drafted.

The rest were made after G4's round (`build/rfc53/g4`), whose agents had
the compiler as amendments 1 to 12 left it. Each closes holes of the round
in this RFC's classes, or brings code in line with RFC 0044 and 0052; they
cost nothing on G2's corpus, and amendment 22 removes 7 of its errors.

13. **D7 through every object (§7).** `mutable` is read through bases,
    through the library's classes of a class with a `mutable` owner (a
    `std::optional`, `std::vector` or `std::pair` of one), and in a `const`
    global; a capture of such an object is never narrowed as read only.
    What a call changes only through a `mutable` member ends the caller's
    borrows and asks nothing of `owner-path`, as §7 says (read into the
    class's own `const` functions, the wider reading gave 30 `owner-path`
    errors in yaml-cpp and tinyxml2; read at the caller, it removed
    tinyxml2's two of S0 and gave one other).
14. **A binder of a view of an erased callable is not read only (§1).** A
    functor over a `const std::function *` calls what the function holds.
15. **D6, as built (§7).** The program's code also runs in a constructor,
    destructor or assignment the program wrote, of a local, a temporary or
    an object a library function makes, copies or destroys (`push_back`,
    `reset`); in a call through a pointer to a function; and in a library
    function given a function (`std::invoke(flip)`, an algorithm's
    comparator). Each ends the borrows into the globals. A view a function
    of the program returns with nothing it could point into (a struct of a
    reference to a function's `thread_local`) is at the globals too.
16. **A view of a view (§1, §5; RFC 0052 §4.1).** What a call reaches
    through a view, a callable or the object of a member function includes
    what the local views it names point to, transitively: a holder of an
    `M &`, given to a function or called, reaches what `M` refers to.
17. **What erases its contents gives copies of them (§5).**
    `std::any_cast<T>(a)` by value points where `a`'s contents do.
18. **An assignment is no accessor (RFC 0044 §6).** C++23's libc++ gives
    `std::pair` and `std::tuple` `const` assignments and `swap`s, which made
    the assignment that is not `const` look like one that only gives
    access. A function that returns nothing, or an assignment, is never an
    accessor, whatever twins it has (read on the instantiation, since
    libstdc++'s `swap` returns an `enable_if<…>::type` as written). The
    round's holes that use `std::bind_front` are pinned for libc++ only:
    libstdc++'s does not compile them.
19. **Libraries' objects of references and destructors (§5).** An
    assignment of a library object of references (`std::tuple<V &>`)
    changes what they refer to; a library call that runs a destructor or
    assignment the program wrote (`reset` of an optional of a view) reaches
    what the object's views point to; so does the destructor of a local or
    a temporary of the program's class.
20. **A library object that holds a callable runs it (§1, §4).** A range
    adaptor and its iterators (`std::views::transform`'s, `filter`'s), or
    anything of the library whose type arguments hold a lambda or functor
    of the program with state, or an erased callable, run it at a library
    call given or called on them, unless the call only accesses an element
    (it has a `const` twin): a vector of lambdas gives one and runs none.
    A deleter, comparator or hash without state holds no loan, and a
    library callable counts only through what it holds
    (`std::reference_wrapper<V>` has a call operator for every `V`).
21. **Types deeper than the search follows (RFC 0052 §2).** A class
    nested more than 64 levels deep is taken to borrow and to write
    through what it holds; it was taken to borrow nothing.
22. **A C library's struct is no view (§5).** `std::unique_ptr<std::FILE,
    Closer>` owns a `FILE`, whose pointers point into what the C library
    keeps (X-A1): a C struct of a system header is not a view for D4. This
    removes kv's 7 errors of S0 (its `Logger` holds one).

## Measured gates

darwin-arm64, Homebrew LLVM 23.1.0, main at `0af7123f` (RFC 0052, PR 79),
2026-11-22. G2's baseline is main's frozen compiler (`build/rfc53/main-tc`);
the agents of G3 and G4 had a frozen copy of this branch's compiler as
amendments 1 to 12 left it (`build/rfc53/tc`). Evidence is in
`build/rfc53/` (not committed). The library grows by 794 lines of code
(29,544 to 30,338; 1,027 lines in all), in `SafeCxxFlow.cpp` and
`SafeCxxKinds.h`.

**G1, RFC 0052's round: passes.** All 90 programs are rejected: the 47
lifetimes holes, the 24 types holes and the 19 programs the checked build
stopped (`s0/holes.sh`), pinned by `test/Safe/cxx-loans-callables.cpp`,
`-kept`, `-containers`, `-results` and `-objects` (written from the round's
files by `s1/gen-tests.py`). `std::as_writable_bytes` stays out, as §1 of
_Bugs deliberately not caught_ has it.

**G2, correct code: passes.** 10 new errors in 10,413 safe functions
against main (0.96 per thousand; D8 allows 2), with `s1/measure.py`:

- libzmq's timers (5): a `void *` argument kept in the timer map; `void *`
  reaches every type (RFC 0052 D2), so the map is a container of views of
  the local passed. No fix is written; the conversion passes the timer's
  own storage.
- kv (1): the address of a local `Server` kept in a global for the signal
  handler, which is the bug the rule is for; G3's agent replaced it with a
  pipe.
- yaml-cpp (3): the iteration over a node's children continues after a
  call given one of them, which the node's iterator values may reach (two);
  a pointer to a local indent marker kept in the scanner's stack of
  pointers. Fixes: collect the children first; keep the marker in the
  stack.
- tinyxml2 (1): a borrow of a new attribute, taken from the element, ends
  at a `const` call of the element, whose base has `mutable` strings (D7,
  read at the caller). Fix: take the attribute after the call.

**G3, the loop: passes.** Fresh agents with the frozen compiler and RFC
0052's brief made the two trial programs, as written, safe with all tests
passing in both builds: interp in about 4 minutes (limit 9, 1.5 times RFC
0051's 6; RFC 0052's took 10.5) and kv in about 12 (limit 19.5; RFC
0052's 13). Both stay free of errors under the final compiler. interp's
agent reported no false rejection of this RFC's; kv's reported one,
`executor.execute_line(line)` making a named `Executor` (which holds a
`Store &`) a view of `line` (RFC 0052 §7's widening, not this RFC's), and
four of other RFCs.

**G4, a fresh round: fails.** Two fresh agents, briefed as RFC 0052's and
told what both RFCs close, wrote 226 probes: 74 holes (48 lifetimes, 26
types), every one rerun (`g4/rerun.sh`). Amendments 13 to 21 reject 62 of
them (`test/Safe/cxx-loans-round-lifetimes.cpp`, `-round-types`). These
12 are still accepted:

- a polymorphic object made from a borrow and held in a library container
  (`std::vector<std::unique_ptr<Base>>`, `std::optional` of one): its
  virtual calls reach nothing (4);
- a `std::reference_wrapper` of a polymorphic object or of a view (`rb.get().run()`,
  `r.get().flip()`) (2);
- `std::tie`'s tuple assigned (1);
- an optional of a class with a destructor that writes through its
  reference, reset: such a class is an owner (RFC 0044), not a view (1);
- a class derived from a library's polymorphic base (`std::exception`)
  (1);
- a view nested 65 or more levels deep (2);
- a span over an array of reference wrappers (1).

The round also found no program the rules accept that the checked build
stops.

**G5, compile time: passes.** With Release compilers of main and this
branch, interleaved, the median of 5 CPU times per file (`g5/g5.py`): 0.999
of main's (interp made safe 0.991, kv 1.000, the json port 0.993, RFC
0044's files 1.000); limit 1.03.

**G6, same code: passes.** Of 512 objects (G2's corpus, `test/cases`,
`test/Guards`), 375 are byte-identical to main's Release compiler's, and
77 differ only where two compiles by main itself differ (RFC 0052's
`__const` table order, and paths under `-g`); 5 compile under main only
(the new errors), 55 under neither (`g6/g6.py`, `g6/recheck.py`).

Status stays **Accepted**: G4 fails.
