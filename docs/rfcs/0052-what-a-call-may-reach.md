# RFC 0052: What a call may reach

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-21
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes, for Safe C++,
  [RFC 0044](0044-safe-cxx.md) §6 rule 3's reading of `const`, §9's
  _Results_, and its implementation amendments 3 (a reference member's
  object as a root nothing ends), 4 (results only into arguments of a
  related type) and 5 (a class keeps a borrow only through a reference
  or a library-view member). Narrows (X6) of the guarantees page by the
  callables it lists. Adds nothing to Safe C, the guards or the runtime.

## Summary

Safe C++ decides which borrows a call ends from a list: a non-`const`
member function of an owner, an owner passed by non-`const` reference.
Anything the list does not name is taken to change nothing. On
2026-11-21 an adversarial round of about 290 safe programs found 24 that
compile as safe C++ with no error and then write a string into a live
`long`, redirect a heap pointer, or read a dead frame. Each one used a way
to change an object that the list does not name:

- calling a lambda that captured the object;
- a reference a helper returned into its argument;
- a write through a reference member or a `std::reference_wrapper`;
- a struct holding a raw pointer to a local.

Rust has no such list. A borrow survives a call only if the call cannot
reach what it points into, and "cannot" is the default the programmer has
to earn. This RFC gives Safe C++ that default, decided as everything in
WeaveC is, inside one function and from types:

- **Reach.** Whether an object of one type can hold or point to an object
  of another is read from the types: through bases, members, elements and
  pointees, with `void *` reaching everything and bytes reaching only bytes
  (D2).
- **Results.** A reference, pointer or view a call returns borrows from
  every argument whose type reaches it, not only one of a related type
  (D3).
- **Views and references.** A call through, or with, a view reaches what
  the view points to, whatever the view's own `const`. A change made
  through any borrow is a change of what it points into (D4).
- **Callables.** Calling a lambda the function can see, or passing it to
  any call, ends the borrows into what its body may change. Passing a
  callable beside a borrow of what it may change is `call-alias` for every
  callee, as Rust checks a closure at the place it is made. A callable
  that is kept holds no borrow (D5, D6).
- **Values built from borrows.** A value whose members could hold a
  borrow of what it was built from is a view of it, raw pointer members
  included. A view of a local is not returned (D7).

Measured before anything else was built, a 155-line prototype of the
first part of results and callables rejects 14 of the 24 lifetime holes
and 3 of the 9 borrow holes of the round's types area. It gives **no new
error** on 157 files of correct safe C++: RFC 0044's 90, RFC 0046's 40,
and the two trial programs of RFC 0051 made safe. All 44 `test/Safe`
tests still pass. The rest of the design closes the remaining holes; S0
measures them the same way before S1 starts, and returns to the owner if
they cost more than D8 allows.

Nothing changes at run time: these are rules, and a unit that compiles
under them compiles to the same code.

## Motivation

### The round

The probe round of 2026-11-21 (`build/probe-2026-11-21/`, `FINDINGS.md`)
gave four agents one area each and the frozen compiler of main at
`fecadb17`. Their brief was to write safe programs that compile with no
error and do something memory-unsafe or undefined with no `weavec:`
report. In Safe C++'s lifetimes area 24 of 81 programs did. Twenty of
them get past the checked build too: the object is alive when it is
misused, so neither the shadow nor the quarantine has anything to say.
Most use `std::variant<std::string, long>`, because a borrow into the
string alternative that outlives a switch to `long` turns a string write
into a write of the `long`'s bytes. ASan sees nothing either; the probes
print the impossible value.

| Class (FINDINGS.md) | Programs | What the flow does not see |
| --- | --- | --- |
| Calling a closure | 10 | a lambda, a `std::function`, `std::visit` or a helper that runs a callback changes what it captured |
| Results | 8 | `char &first(std::string &)` returns into its argument; `result()` links it only to an argument of a related type |
| Through a reference | 2 | `struct M { V &v; }`'s `m.flip()`; `std::ref(v).get() = K` |
| Returned values | 4 | `R{&x}`, the `shared_ptr` aliasing constructor, `views::all` of a local, a returned `initializer_list` |

The types area adds the same classes:
- a `std::any_cast` pointer;
- a write through a `std::span` element or a `reference_wrapper`;
- a variant two struct levels down in a `WEAVEC_ALIASED` class;
- a `shared_ptr` aliasing constructor;
- a returned `initializer_list`.

The worst probe, `lifetimes/p59_sso_helper.cpp`, is 12 lines. A helper
returns `char &` into its `std::string &` argument. The caller grows the
string, and the reference now aliases the low byte of the string's heap
pointer. Bending it makes the string's next write land in a neighboring
string. `p77_ptr_result.cpp` reads the heap pointer out the same way.

### Why patching is the wrong fix

Each class above is a case the list of RFC 0044 §6 did not name. Three of
them are cases its implementation amendments removed on purpose:

- Amendment 3 made what a reference member names "a root of its own,
  which nothing the function does ends".
- Amendment 4 tied a reference result only to "arguments of a related
  type".
- Amendment 5 let a class keep a borrow only through a reference or
  library-view member.

Each removal was made because the body's rule rejected correct code: the
fixed twins of the seeded bugs, `m.find(std::string("k"))` tying an
iterator to its temporary key. Each removal opened a hole. Patching the
24 holes would add 24 cases to the list, and the next round would find
the 25th: the list says what changes an object, and the language has more
ways than any list.

The prototype shows a different trade is available. Amendment 4's reason,
the iterator tied to a temporary key, is a question of types: a map's
iterator cannot point into a `std::string`, because the string reaches
nothing but characters. Asking that question, instead of asking whether
two types are related, keeps the precision amendment 4 wanted and closes
`first`, `p77`, `std::any_cast` and the rest.

### Measured: the prototype (S0, `build/rfc52/s0/`)

A prototype on main (`prototype.diff`, 155 lines in `SafeCxxFlow.cpp` and
`SafeCxxKinds.h`) builds four of the pieces below:

- results by reach (§3);
- calls of lambdas the function sees ending what their bodies change
  (§5.1);
- callables applied at every call they are passed to (§5.1);
- `call-alias` for callables beside what they change, for every callee
  (§5.2).

It took three versions:

1. The first made every call of a callable the function could not see end
   every borrow into what is not the function's own local. It gave 16 new
   errors in 11 of the 157 files. Twelve were the prototype's own bugs:
   - bytes reaching every type, so `std::string`'s buffer reached a
     `Token`;
   - a callable's call ending its own borrow;
   - containers of callables taken for callables.

   Four were the design: loops over a container of stored callbacks that
   call them.
2. The second made a kept callable reach nothing (D6) and a lent one
   reach everything foreign. It left one error: kv's `Index::for_each`,
   which loops over its own map and calls the callback it was lent.
3. The third moved that check to the caller, where `f(v, [&v] { … })` is
   the mistake, and has the callee believe its lent callables (§5.2). It
   gives none.

| | main | prototype |
| --- | --- | --- |
| Lifetimes holes rejected | 0 of 24 | 14 of 24 |
| Types borrow holes rejected | 0 of 9 | 3 of 9 |
| New errors, RFC 0044's 90 correct files | — | 0 |
| New errors, RFC 0046's 40 correct files | — | 0 |
| New errors, interp and kv made safe (27 files) | — | 0 |
| `test/Safe` (44 tests) | pass | pass |

Holes the prototype still accepts, by the part of this RFC that closes
them:

- §4: `p13`, `p31`, the types area's `p18` and `p19`;
- §5.3: `p30`;
- §5.1 for generic lambdas: `p16`, the types area's `p08`;
- §6: `p17b`, `p20`, `p42`, `p45`, `p57`, `p74`, the types area's `p25`
  and `p26`;
- §7: the types area's `p23`.

S0's first task is to build those parts the same way and measure them
against the same 157 files before any of this is written properly.

### What an agent has in Rust that it does not have here

An agent that writes Rust and gets it past the compiler has a program with
no dangling borrow, whatever it wrote. One that writes Safe C++ today has
a program with no dangling borrow of the kinds the list names. The
difference is not visible in an error message: both compilers are quiet.
It is visible in the round, and it is the claim the project makes (the
guarantees page's first sentence) that the round shows to be false. This
RFC makes the claim true for borrows, the largest class of what the round
found.

## Soundness

### What is guaranteed that was not

(X3) _No stale borrow_ holds as stated, without the cases (X6) lists for
callables. Concretely, a safe C++ function does not use a borrow or a
view after:

- a call that could reach the owner it points into, judged by the
  types of the call's arguments, its object and what they point to, not
  by their `const` alone;
- a call of a lambda it can see, or a call it passed that lambda to,
  whose body may change the owner;
- a change made through any reference, pointer or view that points at or
  into the owner, whether the function took it from a variable, a member
  or a call's result.

And it does not return, or keep in memory that outlives it, a value
built from a borrow of its own locals: a struct with a pointer member,
an aliasing `shared_ptr`, a range adaptor, an `initializer_list`.

(X6) loses "a callable that captured `this` or an object a parameter
refers to and is kept by an object that outlives it": a kept callable
holds no borrow (D6).

### What it rests on

- **X-A1, read as types say.** Code that is not safe honors its
  declarations, and a result points only into arguments whose types reach
  it. A library function that returns into an argument of a type that
  cannot hold the result's type breaks the C++ object model, not only
  this rule.
- **X-A2, extended to callables.** The arguments with which code that is
  not safe calls a safe function do not overlap as `call-alias` forbids,
  callables included: a callable passed beside a borrow does not change
  what the borrow points into. A safe caller is checked for it (§5.2).
- **No object in bytes.** Safe code makes no object in a byte buffer
  (placement `new` is `raw-owner`, `pointer-cast` rejects a cast of
  bytes to another type, `pointer-bytes` rejects bytes written into a
  pointer), so a `char` buffer reaches only characters. The library's
  storage that does hold objects (`std::any`, `std::function`,
  `std::optional`, `std::variant`) holds them through `void *` or as
  union members, which reach them.

### Bugs caught

Each of the round's 24 lifetimes holes and the types area's borrow holes
(H2 to H5, H9, H10), as `test/Safe/cxx-reach-*.cpp`. Among them:

```cpp
char &first(std::string &s) { return s[0]; }
void grow(std::string &s) {
  char &c = first(s);
  s.append(100, 'g');
  c = 'X'; // stale-borrow: 'c' points into 's', which 'append' changed
}
```

```cpp
using V = std::variant<std::string, long>;
void f(V &v, const std::function<void()> &cb) {
  std::string &s = std::get<std::string>(v);
  cb();      // believed not to reach v (X-A2)
  s[0] = 'Z';
}
void g() {
  V v = std::string("hello");
  f(v, [&v] { v = 1L; }); // call-alias: the callable may change 'v',
                          // which is passed with it
}
```

```cpp
struct M { V &v; void flip() { v = 1L; } };
void h(V &v) {
  M m{v};
  std::string &s = std::get<std::string>(v);
  m.flip();   // m is a view of v: the call reaches v
  s[0] = 'Z'; // stale-borrow
}
```

```cpp
struct R { long *p; };
R make() { long x = 5; return R{&x}; } // escape: R is a view of 'x'
```

### Bugs deliberately not caught

- **Threads.** The thread area's holes (`std::atomic<T *>`,
  `atomic_ref`, slot-loan counters that wrap, a `stop_callback`, a lambda
  made under a lock and run after it, a `static` in a generic lambda, a
  locked class with a `WEAVEC_ESCAPES` member) are the same question asked
  of a thread start: what can the thread reach? They are the subject of
  the next RFC, which will reuse §2.
- **Shared ownership.** A change through another holder of a
  `std::shared_ptr<T>` stays the hole RFC 0044 §8 opens on purpose
  (_Unresolved question_ 2 there).
- **Safe C, the link and the fast mode's beliefs.** Safe C's bounds from
  the enclosing object, `definition-mismatch`'s missing records, and the
  fast mode's escapes that the checked build catches belong to other
  changes.
- **Code that is not safe.** A guarded function that keeps a callable it
  was lent, or returns into an argument whose type cannot hold the
  result, breaks X-A1 or X-A2 and is trusted, as today.

### Accepted rejections

The defaults reject correct code that the list accepted. The ones the
design expects:

- **A helper with several reference parameters of reachable types**
  returns a borrow of all of them. `const std::string &pick(const
  std::string &a, const std::string &b)` ends when either string changes.
  The fix is `[[clang::lifetimebound]]` (`WEAVEC_LIFETIMEBOUND`) on the
  parameter it returns from. The fixer proposes it when every `return` of
  the function names that parameter (evidence E8, §9).
- **A kept callable that captures `this`** (D6): a
  `std::function<void()>` member initialized with `[this] { … }`, a
  callback registered in an object of another class that captures `this`.
  The fix is to capture what the callable needs by value, a
  `std::weak_ptr` or `std::shared_ptr` to the object, or to pass the
  object as the callable's argument. S0 counts how many of the 157 files
  and the RFC 0049 conversions do it.
- **A view whose `const` was relied on.** A `const std::span<T>` or a
  `const` lambda passed to a function that takes it by `const &` now
  reaches its elements and captures. The fix is a span of `const T`, or a
  lambda that captures by `const &` (`[&v = std::as_const(v)]`).

### Assumptions

X-A1 to X-A7 as the guarantees page states them, with X-A1 and X-A2 read
as above. The rules remain local to a function (RFC 0035 §6's requirement
for anything that rests on analysis). Reach is a property of types, a
callable's changes a property of a body in the function, and a call's
effect a property of its declaration.

## Detailed design

### 1. The principle (D1)

A borrow survives a call only if nothing the call is given can reach the
owner it points into. A borrow survives a change made through another
borrow only if the two cannot point into the same owner. Every "can" is
decided from types and from the function's own code. Where the function
cannot see (an erased callable it was lent, a function of another unit),
the declaration decides, and the declaration's types are read for what
they could hold, not for what their names suggest.

Everything below is in `lib/Frontend/SafeCxxFlow.cpp` and
`SafeCxxKinds.h`. No rule is added; `stale-borrow`, `call-alias` and
`escape` gain cases, and their pages in `resources/safe/rules.md` gain
examples.

### 2. Reach (D2)

`CxxKinds::reaches(given, target)`: whether an object of type `given`
can be, contain, or point to an object of type `target` (both without
references and `const`):

- `given` and `target` are the same type, or one derives from the other.
- `given` is an array of, or a pointer (not to a function) to, a type that
  reaches `target`.
- `given` is a class any of whose bases or members reaches `target`; a
  union reaches what any of its members reaches.
- `void` reaches every type (a `void *` member erases what it points to).
- `char`, `unsigned char` and `std::byte` reach only those three.
- An incomplete class, and a chain deeper than ten levels, reach every
  type.

The result is cached per pair. It follows the library's classes through
their members as it does the program's, so `std::string` reaches `char`,
`std::vector<T>` reaches what `T` reaches, `std::any` and
`std::function` reach every type, and `std::map<K, V>`'s iterator
reaches its node, `K` and `V`, but not a `std::string` key passed to
`find`.

### 3. Results (D3)

RFC 0044 §9 stands for a result that is a view. For a reference or
pointer result, amendment 4's "only into arguments of a related type"
becomes "into every argument whose type reaches the result's referent
type":

- **A function that is not a member.** Its result points into each
  reference argument, and where each pointer or view argument points,
  whose type reaches the result's. It points _at_ an argument of the
  result's type, as today.
- **A member function.** Its result points into its object (as today)
  and also into each reference argument whose type reaches the result's.
  `m.find(std::string("k"))` stays untied to its key; `pick(a, b)` as a
  member is tied to both.
- **`[[clang::lifetimebound]]`** (`WEAVEC_LIFETIMEBOUND`) narrows as
  today: when any parameter has it, the result points only into those
  (and `this` when the function has it).
- **A safe function is checked against the same reading**, as today: it
  returns a borrow only of what its declaration allows.

### 4. Views and references (D4)

`const` on an owner is deep: the library's owners give only `const`
access to what they hold through a `const` object, and the rules check a
safe class's `const` member functions (`owner-path`). `const` on a view
is shallow. A `const std::span<T>` writes its elements, a lambda's
`operator()` is `const` and writes what it captured by reference, and a
`const std::reference_wrapper<T>` gives a `T &`. So:

1. **A call reaches what a view points to.** For rule 3, an argument that
   is, or holds, a view reaches what the view points to, whatever the
   parameter's `const`, unless the view's referent is `const`
   (`std::span<const T>`, `std::string_view`, a lambda that captures
   nothing by non-`const` reference). The object of a member call is an
   argument too: `m.flip()` on a view `m` of `v` reaches `v`.
2. **A change through any borrow is a change of its roots.** An
   assignment through, or a non-`const` call on, a reference, a pointer, a
   view's element or a call's result ends the borrows into every owner it
   points at or into (rule 2, today applied only to borrows held in
   locals). `std::ref(v).get() = K` and `span[0] = K` end the borrows into
   `v`.
3. **A reference member's object is what the view points to, for the
   view's holder.** Amendment 3 stands inside the class (what `M::v`
   names is a root of `flip`'s own, which only its caller sees). Its
   caller sees `m` as a view of `v` (§7 of RFC 0044, _Made_), and by 1 a
   call on `m` reaches `v`.

### 5. Callables (D5, D6)

#### 5.1 What a callable the function sees may change

A lambda written in the function, or a local initialized from one
(directly or through an erased type, `std::function<void()> f = [&] {
… };`), has a body the function can read. What the body may change is
computed as `lambdaChanges` does today, over every instance of a generic
lambda's call operator, and grows by this RFC's reading of calls (§4). It
covers a capture by reference that the body changes, passes on as one
that changes, or reaches through a view it changes, and `*this` when the
lambda captures `this`.

- **A call of the callable** ends the borrows into what it may change.
- **A call the callable is passed to** (any function, the library's
  algorithms and `std::visit` and `std::invoke` included, and the
  program's own) ends the same borrows: the callee may call it. A callee
  that only stores it is §5.3's.

#### 5.2 Passing a callable beside what it may change (`call-alias`)

RFC 0044 §9 checks a callable passed to a library function beside a
borrow into what the callable changes. This RFC checks it for every
callee, and beside the object itself as well as a borrow into it:
`f(v, [&v] { v = 1L; })` is `call-alias` when `f` takes `v` by reference,
and so is `obj.visit([&obj] { … })`. Rust rejects the same programs where
the closure is made.

In exchange, a callee believes (X-A2) that a callable it was lent reaches
nothing its other parameters and `this` reach. Calling it ends no borrow
of theirs. The visitor that loops over its own container and calls the
callback (`for (auto &e : items_) fn(e);`) stays correct and accepted.
The prototype measured both sides of this choice (_Measured_).

#### 5.3 A kept callable holds no borrow (D6)

RFC 0044 amendment 6 already stops a safe caller from passing a callable
that captures a local by reference where it is kept. This RFC completes
it. A callable that is kept (stored in a member, an element, a global or
a `static`; passed for a parameter of erased type taken by value; returned
as an erased type) captures nothing by reference, and neither `this` nor
a pointer. It captures values and owners, a `std::shared_ptr` or a
`std::weak_ptr` included. Calling a kept callable then reaches only what
those reach, which no borrow of the caller points into. The callable
stored in a member of the class that iterates it is §5.2's visitor again.
A kept callable that captures `this` is `escape`, with the fix
`[weak = weak_from_this()]` for a class that is shared, and otherwise a
note naming the two others (capture what the body uses by value; take the
object as the callable's argument).

### 6. Values built from borrows (D7)

Amendment 5 kept a borrow in a class only through a reference member, a
`WEAVEC_BORROWED` member or a library view or erased member, and so a
raw pointer member kept none. This RFC reads the members by reach. A
value built by a constructor, an aggregate initialization, a conversion
or a library factory (`std::ref`, `std::views::all`, the aliasing
`shared_ptr` constructor) is a view of each argument that one of its
_borrowing_ members could point to or into:

- a reference member;
- a pointer member of a class that is not an owner;
- a library view or erased member;
- a member of a class that is itself such a view.

A pointer that an owner keeps of its own heap block (`std::string`'s
buffer, `std::vector`'s elements) does not borrow. Reach decides "could":
`Logger(const std::string &prefix, Sink &sink)` with a `std::string`
member and a `Sink &` member borrows the sink and copies the prefix, as
amendment 5 wanted.

A `std::initializer_list` is a view of the array the compiler made for
it, which lives as long as the full expression or the variable it
initializes. It is not returned, stored or kept past either; a function
that returns one is `escape`. A shared pointer made by the aliasing
constructor is a view of what its second argument points into, and an
owner of nothing.

Returning, storing and keeping these views follow RFC 0044 §7 unchanged.
`R{&x}` returned from the function that declares `x` is `escape`, in
both modes, at compile time; today only the checked build stops it, as
a stack use after return.

### 7. Aliased classes (fix)

RFC 0047's check that a `WEAVEC_ALIASED` class owns no storage that
switches type in place looks one level deep (types area, `p23`). It
follows members to any depth, through the classes reach follows.

### 8. What does not change

- **Run time.** No pass, no runtime entry and no report kind changes. A
  unit that compiles under both main and this RFC compiles to the same
  object (gate G6).
- **Safe C, the guards, the fast and checked modes, the ledger and the
  audit.** The ledger's `errors` list carries the new cases with their
  fixes as RFC 0049 has it; its version does not change.
- **The annotations.** `WEAVEC_LIFETIMEBOUND` (Clang's
  `[[clang::lifetimebound]]`) already exists and is read as RFC 0044 §9
  says.

### 9. Fixes the errors carry (RFC 0049)

| Error | Fix | Certainty |
| --- | --- | --- |
| `stale-borrow` after a call whose result reached several arguments | `WEAVEC_LIFETIMEBOUND` on the parameter every `return` names | likely (E8: the function's returns, in the unit) |
| `call-alias`, a callable beside what it changes | none written; the note says to collect the changes in a local and apply them after the call | — |
| `escape`, a kept callable capturing `this` | `[weak = weak_from_this()]` and a lock in the body, for a class that derives from `enable_shared_from_this` | choice |
| `escape`, a returned view built from a local | none written; the note names the local | — |

E8 is new evidence: a function defined in the unit all of whose `return`
statements return a borrow of the same parameter.

### 10. Stages and gates

- **S0. Measure the rest (before S1).** Extend the prototype to §4, §5.1's
  generic lambdas, §5.3 and §6, and measure against the 157 files, the
  `test/Safe` tests, RFC 0049's conversions (tinyxml2 and yaml-cpp made
  safe, libzmq's and nng's converted functions) and the json port.
  Report:
  - the holes rejected;
  - the new errors per thousand functions;
  - which of them a fix covers.

  If the new errors exceed D8's limit, return to the owner with the list
  before S1.
- **S1. Reach and results** (§2, §3).
- **S2. Views and references** (§4).
- **S3. Callables** (§5): what a callable changes, `call-alias` for every
  callee, kept callables.
- **S4. Values built from borrows and `initializer_list`** (§6), and §7.
- **S5. The fixes** (§9), the rules' pages, the guide's "Borrows"
  section, the guarantees page ((X3), (X6)), the roadmap.

Gates:

- **G1. The round.** Every Safe C++ borrow hole of the round (lifetimes
  24, types H2 to H5, H9, H10) is rejected, each pinned by a
  `test/Safe/cxx-reach-*.cpp` test.
- **G2. Correct code.** New errors on the 157 files, the `test/Safe`
  tests' accepted lines, RFC 0049's conversions and the json port stay
  within D8's limit. Each is listed in the PR with its fix or the reason
  there is none.
- **G3. The loop.** An agent with the frozen compiler makes RFC 0051's
  two trial programs, as written, safe in no more than 1.5 times RFC
  0051's time (6 and 13 minutes), with all tests passing.
- **G4. A fresh round.** Two new agents, briefed as the round's lifetimes
  and types agents were and told what this RFC closes, find no program of
  those classes that compiles as safe and misuses a borrow unstopped.
  What they find of other classes is reported and does not fail the gate.
- **G5. Compile time.** The trial programs, the json port and RFC 0044's
  files compile within 1.03 of main's time with a Release compiler.
- **G6. Same code.** Every object of the corpus that compiles under both
  is identical to main's, modulo the ledger.

## Annotation surface

None. `WEAVEC_LIFETIMEBOUND` exists. D6 changes what a kept callable may
capture, which is a rule, not an annotation.

## Diagnostics

No new rule and no new run-time report. New messages under existing
rules:

- `stale-borrow`: `'<b>' points into '<o>', which the call to '<f>' may
  reach through '<argument>'`, with a note at the call.
- `stale-borrow`: `'<b>' was taken from '<o>' before the call to '<f>',
  which may call '<callable>', which changes '<o>'`.
- `call-alias`: `'<callable>' may change '<o>', which is passed with it`.
- `escape`: `'<callable>' is kept by '<keeper>' and captures '<what>'`.
- `escape`: `'<value>' holds a borrow of '<local>' and is returned`.

Each is pinned by a `test/Safe/` test with its note, as RFC 0044's are.
`--weavec-explain stale-borrow`, `call-alias` and `escape` gain the
examples of _Bugs caught_.

## Drawbacks

- **More correct code is rejected.** The prototype found none in 157
  files. The rest of the design is the part that touches idioms people
  rely on: `const` spans, kept callbacks that capture `this`, helpers
  with several reference parameters. S0 and D8 exist for it.
- **A larger flow.** About 600 lines in `SafeCxxFlow.cpp` and
  `SafeCxxKinds.h`, by the prototype's rate. The flow is already the
  largest file of the checker.
- **Less of the guarantee rests on the caller.** A callee believes more
  of its lent callables (X-A2). Code that is not safe that breaks it is
  trusted, as for every other overlap today.

## Alternatives

- **Patch the 24 holes.** Smaller, and the next round finds the 25th: the
  three amendments that made most of them were each reasonable on their
  own. Rejected for the reason in _Motivation_.
- **Exclusive borrows, as Rust.** A non-`const` borrow excludes every
  other borrow of the same owner for its life. That is sound and simple,
  and it rejects the most common correct C++: `auto &a = v[i]; f(v);`
  where `f` takes `const std::vector<int> &`, and two references into one
  container. It would undo RFC 0044's choice to follow what changes, not
  what aliases, which the agents' conversion times depend on.
- **Make lent callables end everything in the callee.** The prototype's
  first version. Sound, and it rejected four correct visitors out of 157
  files; the caller-side check of §5.2 rejects none and is what Rust does.
- **Let calls of kept callables end every borrow of `this`** instead of
  D6's rule. That keeps `[this]` callbacks and rejects the loop over a
  class's own callbacks that calls them, which the 157 files have four
  times (the prototype's first version).
  D6 rejects the callback once where it is made, with a fix, instead of
  at every loop that runs it.
- **Ask Clang.** Clang's `-Wdangling` family and `lifetime_capture_by`
  warn about some of these, with no promise that the absence of a warning
  means anything. The project's claim needs the promise.

## Prior art

- **Rust.** The borrow checker's exclusivity, closures' capture modes
  (a closure that captures `&mut v` is itself a mutable borrow of `v`),
  the `'static` bound on stored callbacks (`thread::spawn`, `Box<dyn Fn +
  'static>`), and lifetime elision (RFC 141), whose default ties a
  result to its one reference input. This RFC takes the defaults, not
  the lifetimes: a function's signature does not name lifetimes, so the
  defaults are wider and the fix is `lifetimebound`.
- **The C++ Core Guidelines' lifetime profile** (Sutter, P1179R1):
  points-to sets, and a default that a returned pointer derives from
  every reference or pointer parameter of a compatible type. §3's reach
  is the same default, with "compatible" made precise by members.
- **Clang's `[[clang::lifetimebound]]` and `lifetime_capture_by`.** The
  first is the narrowing D3 reads; the second would let a library say
  which argument a call keeps, and is _Future work_.
- **Circle's Safe C++** (Baxter, P3390): borrow checking with lifetime
  parameters in C++. It is sound, but it needs a new standard library and
  new syntax. WeaveC's constraint is to keep both.

## Unresolved questions

1. **D8's limit.** The draft proposes 2 new errors per thousand safe
   functions, each with a fix or a one-line reason. The number is a
   proposal: the prototype's parts gave none, and the parts S0 adds are
   the ones that touch common idioms.
2. **`const` views at call boundaries.** §4.1 makes a `const
   std::span<T> &` parameter reach the elements. Should the library table
   be allowed to say a function reads only (`std::accumulate(span)`)? The
   library's functions are trusted with what their specification allows
   (X-A4), which already covers its algorithms. The question is for the
   program's own functions.
3. **Template callables.** `template <class F> void each(F &&f)` is lent
   `f` (§5.2), and `f` is a parameter of the template's instance, whose
   type is the lambda's. Should the instance read the lambda's body? It
   can: it is the same unit. The draft says no, so that every instance is
   checked alike.
4. **Virtual calls** are judged by the declaration they are made through,
   as today. Reach from `this` covers the override's members only through
   the base's declared types. Is a derived class's extra member a hole?
   S0 should write the probe.

## Future work

- **What a thread may reach** (the next RFC): the thread area's holes,
  with §2's reach applied to what a thread start, an atomic of a pointer
  and a loan give a thread.
- **The link's records**: templates, `-l` archives, `typedef struct {…}`
  and C's nested structs for `definition-mismatch`, as bug fixes to RFC
  0051.
- **Safe C's field bounds** from the enclosing object, and a count
  changed through its address in the fast mode.
- **`lifetime_capture_by`** read from the library's declarations, so that
  `push_back(std::string_view)` says what it keeps.

## Decisions to accept

Accepted as drafted on 2026-11-21.

- **D1. Scope.** Safe C++ borrows only. Threads, the link, Safe C and the
  fast mode's beliefs are later work.
- **D2. Reach by types**, as §2 defines it: `void *` reaches every type
  and bytes only bytes.
- **D3. Results borrow by reach**, replacing amendment 4's related
  types; `lifetimebound` narrows.
- **D4. `const` on a view is shallow**, and a change through any borrow
  is a change of its roots; replacing rule 3's reading of `const` for
  views, and amendment 3 for a view's holder.
- **D5. Callables:**
  - a lambda's changes apply where it is called and where it is passed;
  - `call-alias` holds for every callee;
  - a callee believes its lent callables reach nothing its other
    parameters reach.
- **D6. A kept callable holds no borrow**: no reference, no `this`, no
  pointer captured.
- **D7. Values built from borrows by reach**, replacing amendment 5; an
  `initializer_list` is not returned or kept.
- **D8. The cost limit.** New errors on G2's corpus are at most 2 per
  thousand safe functions, each with a fix or a stated reason. S0 returns
  to the owner if the design exceeds it.
