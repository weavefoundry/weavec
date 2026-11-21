# RFC 0053: Callables and containers by their loans

- **Status**: Draft
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
void go(Base &b);
void through(V &v, Base &b) {
  std::string &s = std::get<std::string>(v);
  go(b);       // reaches what 'b' points to, as a view: unknown, so
  s[0] = 'Z';  // only if 'b' could point to 'v' (a derived class's member)
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
- **A virtual call on an object the function was handed.** Its borrows of
  what the object could reach end at the call. The fix is to call through a
  `final` class, or take the borrow after the call.
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

A call of a virtual member function that is not `final`, on an object whose
dynamic type the function did not make (a reference or pointer parameter,
what a smart pointer to a base holds), reaches what the object's loans
reach, as a view of the program's class does under RFC 0052 §4.1. A
conversion of a smart pointer or a reference to a base keeps the loans of
what it converts. This answers RFC 0052's _Unresolved question_ 4.

### 7. Globals, `mutable`, owners (D6, D7)

- A borrow whose root is a global that is not `const` (a namespace-scope
  variable, a `static` member, a `thread_local`, a function's `static`), or
  that a function of the program returned with no argument it could point
  into, ends at every call of a function of the program and at every
  change through another such result.
- A class with a `mutable` member that holds owners is changed through a
  `const` reference or a `const` member function as through one that is not
  `const`.
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
- **Virtual calls become barriers** for borrows of what the object could
  reach. Code that calls through interfaces while holding borrows of objects
  the interface's implementations could reference will need to take the
  borrow again.
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
3. **Interfaces.** D5 makes a virtual call reach what the object's loans
   reach; for a reference parameter that is every owner the caller's
   types reach. Should a class be able to say its implementations hold no
   references (`final` aside)?

## Future work

- **Threads**, as RFC 0052's _Future work_ has them.
- **Bytes**: `std::as_writable_bytes` and the spans of bytes over objects
  that hold pointers, with RFC 0051's and PR 78's rules.
- **A variant of trivially destructible alternatives** (RFC 0052
  amendment 12).

## Decisions to accept

- **D1. Callables by their loans:** a call of any callable value, or a call
  it is passed to, ends the borrows into what its loans reach; a lambda's
  body the function wrote narrows only by proof that a capture is read.
- **D2. `call-alias` and kept callables by loans**, for every callable
  type.
- **D3. A library function given a callable** is the program's for RFC
  0052 §4.1.
- **D4. The library's containers of views are views** of what they are made
  from, however they are made.
- **D5. A virtual call that is not final** reaches what its object's loans
  reach.
- **D6. A borrow into a mutable global** ends at every call of a function
  of the program.
- **D7. `mutable` makes `const` shallow; `= default` is no owner's
  destructor.**
- **D8. The cost limit** stays RFC 0052's: at most 2 new errors per
  thousand safe functions on G2's corpus, each with a fix or a stated
  reason, measured against RFC 0052's compiler. S0 returns to the owner if
  the design exceeds it.
