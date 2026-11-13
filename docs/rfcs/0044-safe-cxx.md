# RFC 0044: Safe C++

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-12
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Carries out what
  [RFC 0041](0041-safe-c.md) §12 and [RFC 0043](0043-ownership.md) §13
  deferred ("Safe functions are C only for now"; "the C++ RFC should be
  written against them"). Supersedes RFC 0041 §12 and the rule `cxx`.
  Extends the rules `owner-moved`, `stale-borrow`, `call-alias`,
  `owner-path`, `escape`, `pointer-cast`, `pointer-bytes`, `index-single`,
  `union-pointer`, `int-to-pointer`, `variadic`, `no-asm`, `unsafe-block`
  and `bad-annotation` to C++, and adds `raw-owner` and `not-modeled`.
  Amends the driver's flags, `weavec.h`, the library table, the enforcement
  ledger and the audit. Changes no guard, no report kind and nothing in
  the runtime; safe C is unchanged.

This RFC was written at the request of the project owner on 2026-11-12,
who asked a fourth time for the single change that would do most to make
an AI rewrite into C or C++ as attractive as an AI rewrite into Rust,
"being able to iterate with memory-based compiler errors", to be
implemented next, with breaking changes and the removal of old code
welcome. The measurements in _Motivation_ were taken on main at v0.24.0
(`6bc77f69`), darwin-arm64, Homebrew LLVM 23.1.0; the probes are kept in
`build/rfc44/probe/` with the commands that produced each number.

The owner accepted the design as drafted on 2026-11-12, before any of it
was built, and with it the five decisions that were theirs to make. They
are marked **(D1)** to **(D5)** where they are argued, and collected here.
The thresholds of §15 and the _Unresolved questions_ are still open, and
S0 is where they are settled; questions 2 and 3 may reopen (D3) with what
the ports and conversions measure.

- **(D1) Safe C++ is a subset.** A safe function does not write `new`,
  `delete`, pointer arithmetic, a `reinterpret_cast` or a union. Modern C++
  that uses the standard library's owners compiles; C with classes does
  not, until it is rewritten or kept guarded.
- **(D2) The rules are not load-bearing, so this RFC makes nothing
  faster.** A safe C++ function keeps every guard it has today and costs
  what guarded C++ costs (1.90 times the reference, RFC 0039 gate G7).
  Spending the rules is the next RFC, as RFC 0042 followed RFC 0041.
- **(D3) A pointer member is read-only where it matters.** A safe function
  does not change what an object owns through a raw pointer member; it
  does through a `std::shared_ptr`, which is the declared way to share
  state and the one place the rules knowingly do not follow.
- **(D4) The standard library is trusted** to behave as the standard says,
  as safe Rust trusts `std`.
- **(D5) The library's line limit rises** from 17,500 to about 20,500.

## Summary

Safe C gives an agent Rust's loop in C: a mistake with a bound, a
conversion or an owner stops the compile with the rule and the fix. Few
would rewrite a service, a compiler or a game from another language into C.
They would rewrite it into C++, which has the containers, the strings, the
generics and the destructors that C makes an agent write by hand; and the
code that most often considers a rewrite into Rust is C++ already. For C++
WeaveC has only the other half. `weavec-c++` stops every one of twenty
plausible memory bugs when a test reaches it, and tells the agent about
four of them when it compiles, as warnings, because Clang does. `weavec-c++
-fweavec-safe` is an error: "safe functions are C only".

This RFC makes that flag work. A **safe C++ function** is checked when it
compiles, by rules local to the function, over the three kinds of pointer
RFC 0043 defined:

- An **owner** is an object: a local, a member, an element, and what a
  `std::vector`, a `std::string`, a `std::unique_ptr` or a `std::optional`
  holds. Destructors release, so safe C++ writes no release at all, and
  `new` and `delete` are not written (`raw-owner`).
- A **borrow** is a reference, a pointer, an iterator, a `std::string_view`,
  a `std::span` or a lambda that captures by reference, held in a local or
  passed down a call. A type that holds one in a member is a **view**, and
  a view is a borrow wherever it goes. A borrow is not used after the owner
  it points into was destroyed, moved, or changed by anything that is not
  `const`: the reference kept across `push_back`, the `string_view` of a
  temporary, the loop that erases from what it iterates (`stale-borrow`).
  It is not returned or stored where it could outlive what it points into
  (`escape`).
- A **reference** is a raw pointer kept in a member or an element. No rule
  follows it and the guards check it when it is used, as today.

With those come `owner-moved` (an object used after `std::move`),
`call-alias` and `owner-path` as RFC 0043 states them, and the constructs
a safe function does not contain: pointer arithmetic, casts that forge a
pointer, unions, raw allocation.

Nothing at run time changes. The rules reject; every guard, every library
check and the sweep stay where they are, so a hole in a rule is a missed
diagnostic that still stops when it runs. That is RFC 0043's bargain, and
it is what makes it reasonable to check a language as large as C++ with a
checker small enough to read.

Expected result (gates, §15): of a frozen set of temporal bugs seeded into
safe C++, at least 90% of those on owners, borrows and views stop the
compile, where none does today; at least 90% of a frozen set of correct
idiomatic functions compile unchanged; and a port from one specification
into safe C++ and into Rust takes a comparable number of rounds.

## Motivation

### The loop exists in C, and the rewrite will not be into C

RFC 0043's port (its gate G5) is the closest measurement there is of the
owner's goal: one specification, two agents, Safe C and Rust. Safe C
reached 22 of 22 functions safe in two rounds. It also took 4 min 31 s to
Rust's 1 min 31 s. Where that time went was not measured; three
differences between the languages were visible in what the agents wrote,
and none is the checker's to fix:

- **The agent wrote the containers.** The specification asked for a list
  and a hash map by hand in both languages. In a real rewrite the Rust
  agent writes `Vec`, `String` and `HashMap`, and the C agent writes a
  vector, a string builder and a hash map again,
  each with its bounds annotations and its release on every path. In RFC
  0042's port the Rust agent wrote no release at all.
- **C has no generics.** The held-out conversion of c-algorithms kept 40
  of 209 functions guarded, "mostly the AVL and red-black trees' rotations
  … and `void *` adapters" (RFC 0043, gate G4). A container of `void *` is
  how C spells `Vec<T>`, and a `void *` is where Safe C's rules end.
- **C has no destructors.** `owner-leak` exists because C makes the agent
  write each release; 2 of the port's 8 first-round errors were leaks, and
  the one error the final compiler added was another. In C++ and in Rust
  that class cannot be written.

Each of these is a property of C. Safe C can narrow them (RFC 0042 names
typed callbacks and `void *` as next work, RFC 0043 ownership by outcome
and init parameters), and each step would be a language feature WeaveC
invents and no model was trained on. C++ has had all three for decades, and every
model has read a great deal of it.

The second reason is who rewrites. The codebases that move to Rust are
often C++ already (browsers, databases, operating system components),
and otherwise programs in managed languages that want native speed. For the
first group, safe C++ is not a rewrite at all: it is a conversion in
place, a file at a time, of code whose tests, build and callers stay. For
the second, C++ is the target a model already writes fluently. WeaveC
compiles C++ since RFC 0039 (13 corpus projects and 4 programs pass
guarded), and gives none of it a rule.

### Measured

Twenty functions, each with one memory bug an agent could plausibly write
in modern C++ (`build/rfc44/probe/bugs.cpp`; `-std=c++20 -O2`). "Clang"
is the compiler WeaveC is built on with `-Wall -Wextra`, which is what
`weavec-c++` prints today; "lifetime" adds `-Wlifetime-safety` and its
inference flags; "runs" is the guarded program.

| Bug | Clang | lifetime | `weavec-c++` runs | This RFC |
| --- | --- | --- | --- | --- |
| 1. a reference into a vector, kept across `push_back` | — | warning | `heap-use-after-free` | `stale-borrow` |
| 2. the same across a call that grows the vector | — | — | `heap-use-after-free` | `stale-borrow` |
| 3. `push_back` inside a range-`for` over the same vector | — | warning | `heap-use-after-free` | `stale-borrow` |
| 4. an iterator used after `erase` | — | warning | `heap-use-after-free` | `stale-borrow` |
| 5. a `string_view` of a temporary string | warning | warning | `heap-use-after-free` | `stale-borrow` |
| 6. a `string_view` of a local, returned | warning | warning | `heap-use-after-free` | `escape` |
| 7. a reference to a local, returned | warning | warning | `stack-use-after-return` | `escape` |
| 8. a lambda capturing a local by reference, returned | — | warning | `stack-use-after-return` | `escape` |
| 9. a `unique_ptr` used after `std::move` | — | — | `null-dereference` | `owner-moved` |
| 10. a pointer from `get()`, used after `reset()` | — | warning | `heap-use-after-free` | `stale-borrow` |
| 11. a pointer member left pointing at a destroyed object | — | — | `heap-use-after-free` | runs: as today |
| 12. a reference member bound to a temporary | — | warning | `stack-use-after-scope` | `stale-borrow` |
| 13. a reference to a map's element, used after `erase` | — | — | `heap-use-after-free` | `stale-borrow` |
| 14. a `span` over a vector that a call then grows | — | — | `heap-use-after-free` | `stale-borrow` |
| 15. `c_str()` of a temporary | warning | warning | `heap-use-after-free` | `stale-borrow` |
| 16. an index past a vector | — | — | `library-assertion` | runs: as today |
| 17. an empty `optional` dereferenced | — | — | `library-assertion` | runs: as today |
| 18. `new` and two `delete`s | — | warning | `heap-use-after-free` | `raw-owner` |
| 19. a child's parent pointer, the parent destroyed first | — | — | `heap-use-after-free` | runs: as today |
| 20. a pointer into an `unordered_map`, kept in a vector across a rehash | — | — | `heap-use-after-free` | runs: as today |
| **Stops the compile** | **0** (4 warnings) | **0** (11 warnings) | — | **15** |

The reference build prints a result for 18 of the 20 and crashes on two.
AddressSanitizer reports 17. The guarded program stops all 20, each with
the report RFC 0040 gives it. What `weavec-c++` adds when the agent
_compiles_ is nothing: four warnings that are Clang's, and no error. By
Rust's rules (not measured here) 18 of the 20 cannot be written and two
panic when they run (16 and 17, as in the last column).

Under this RFC five stay run-time errors. Two are checks Rust also makes
when the program runs. Three go through a raw pointer kept in memory (11,
19, 20), which is what a reference is: RFC 0043 chose to check those when
they are used, and the guards of C++ already do.

### What Clang has, and why it is not the answer by itself

Clang 23.1 ships a lifetime-safety analysis for C++ (loans and origins
over the CFG, after Polonius) that RFC 0043 measured in C and found
covered the stack only. In C++ it is much further along, and this RFC
would be dishonest not to measure it:

- **It finds 10 of the 20** with `-Wlifetime-safety`, and 11 with
  `-Xclang -flifetime-safety-inference -Xclang
  -fexperimental-lifetime-safety-tu-analysis`. It misses what crosses a
  call (2, 14), what is moved (9), a map's `erase` (13) and everything in
  memory (11, 19, 20).
- **It warns about correct code.** `build/rfc44/probe/fp.cpp` has five
  correct functions; three are reported: two `push_back` calls through a
  reference parameter ("parameter 'v' is later invalidated"), two `+=` on
  a string parameter, and a pointer taken again after the call that
  invalidated it. The probe's own helper `grow` is reported the same way.
  A rule that rejects `v.push_back(1); v.push_back(2);` cannot be an
  error.
- **It is a set of warnings.** Off by default, in groups that are still
  being renamed, with no statement of what a clean compile means, no
  account of what code outside the analysis must keep, and no rule an
  agent can be told in advance.
- **It can be called.** `clang/Analysis/Analyses/LifetimeSafety/` is
  installed with LLVM 23.1: `runLifetimeSafetyAnalysis` takes a
  `LifetimeSafetySemaHelper` whose virtual functions receive each finding.
  WeaveC could run it and print the findings as its own errors. It could
  not fix the false ones, add moves or calls, or say what holds.

`clang-tidy` finds the use after move (9) and two of the dangling views,
as a separate tool. So the pieces exist, as warnings, in three places, at
about half coverage, with false reports on the simplest correct code.
What is missing is what RFC 0043 found missing in C: rules that are
declared, that reject, that an agent can read before it writes, with the
guards under them. S0 keeps Clang's analysis as an oracle: a seeded bug
it reports and the rules accept is a hole to explain (gate G8).

### Why this is affordable now

Three things that were not true before RFC 0043:

1. **The kinds are designed.** Owners, borrows and references, what ends
   a borrow, why `const` matters, `call-alias` and `owner-path`: RFC 0043
   §13 predicted that safe C++ "needs exactly these three kinds", and the
   C++ Core Guidelines' lifetime profile, whose rule §5 of that RFC
   already is, was written for C++ in the first place.
2. **The checker exists.** `SafeOwnership.cpp` is a forward dataflow over
   Clang's CFG with places, states, joins and borrows. C++ needs fewer
   states (there is no `owner-leak`: destructors run) and more forms
   (member calls, temporaries, lambdas).
3. **Nothing has to rest on it.** Guarded C++ is already memory safe at
   run time: 20 of 20. The rules can start as what they are, a way to
   move errors from the test run to the compile, and be judged by how
   many they move.

## Soundness

### Owners, borrows, views and references

In a safe C++ function every pointer, reference and object is one of:

- An **owner**: an object with a destructor to run. A local, a parameter
  taken by value, a member, an element of a container, the object a
  `std::unique_ptr` or `std::optional` holds. An owner owns the objects
  inside it and the heap blocks its destructor releases.
- A **borrow**: a pointer or reference to an object, or into what an owner
  holds, kept in a local, a parameter or a temporary. `T &`, `const T &`,
  `T *`, an iterator, `std::string_view`, `std::span`, a lambda that
  captures by reference. Its validity is decided when the function
  compiles.
- A **view**: an object whose type holds a borrow (§2): a struct with a
  `std::string_view` member, a `std::vector<std::string_view>`, a class
  with a reference member. A view is an owner of its own storage and a
  borrow of what it points into, and the rules follow it as a borrow.
- A **reference**: a raw pointer loaded from a member, an element or a
  global. Nothing is decided about it when the function compiles; every
  access through it is guarded.

### The guarantee

For a program that meets X-A1 to X-A5 below, in every build:

- **(X1) No raw owner.** A safe function allocates and releases heap
  memory only through objects whose destructors release it. It contains no
  `new`, no `delete`, no call that returns or takes a raw owner. A double
  free, a mismatched `delete`, and a leak by a forgotten release cannot be
  written in one.
- **(X2) No use of a moved object.** A safe function does not use an
  object after moving from it, on any path, until it assigns to it.
- **(X3) No stale borrow.** A safe function does not use a borrow or a
  view after an owner it points into was destroyed, moved, assigned, or
  passed to anything that could change what it owns: by the function
  itself, or by a call it made (§6). That includes the end of a local's
  scope and the end of the full expression that made a temporary.
- **(X4) No borrow where the rules cannot follow it.** A safe function
  does not return a borrow or a view of its own locals, and does not store
  one in memory that is not a view's (§7).
- **(X5) No forged pointer.** A safe function does no pointer arithmetic,
  indexes no raw pointer, and performs no cast that makes a pointer from
  an integer or from a pointer to an unrelated type (§4).
- **(X6) What is left.** A temporal error in a safe function can then go
  only through a reference, through an object shared by `std::shared_ptr`
  that another holder changed, through a global, or through a callable
  the function received in a type that hides what it captured (§7). A
  spatial error can only be an index or an iterator out of range. All of
  them stop when they run, by the guards and the library's checks, as
  (M1) to (M4) of the guarantees page say for guarded C++ today.

(X1) to (X5) are facts about the source and hold under any compiler.
(X6)'s last sentence needs `weavec-c++`.

### What gets weaker

Nothing. A safe C++ function is compiled exactly as the same function is
today: the same guards, the same library checks, the same reports. Gate
G7 checks that the object code is the same.

### Accepted false traps

None new; nothing at run time changes.

### Bugs caught

```cpp
// weavec-c++ -std=c++20 -fweavec-safe
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Item { int v = 0; std::string name; };
struct Token { std::string_view text; int kind = 0; };  // a view
struct Cache { Item *last = nullptr; };                 // holds a reference

std::string read_file(int n);
std::vector<Token> lex(std::string_view source);

int grow(std::vector<Item> &items) {
  Item &first = items[0];            // a borrow into 'items'
  items.push_back(Item{});
  return first.v;     // error: 'first' points into 'items', which
                      // 'push_back' may have changed
                      // [weavec-safe:stale-borrow]
}

int moved(std::unique_ptr<Item> it, std::vector<std::unique_ptr<Item>> &all) {
  all.push_back(std::move(it));
  return it->v;       // error: 'it' is used after it was moved
                      // [weavec-safe:owner-moved]
}

std::vector<Token> tokens_of(int n) {
  std::string source = read_file(n);
  std::vector<Token> tokens = lex(source);  // a view of 'source'
  return tokens;      // error: 'tokens' holds borrows of the local
                      // 'source' [weavec-safe:escape]
}

Item *make() {
  return new Item;    // error: 'new' makes a block no object owns
                      // [weavec-safe:raw-owner]
}

struct Subject;
struct Observer {
  Subject *subject = nullptr;        // a reference
  void notify();
};
struct Subject {
  std::vector<Observer *> observers;
  void remove(Observer *o);
  void notify_all() { for (Observer *o : observers) o->notify(); }
};
void Observer::notify() {
  subject->remove(this);
                      // error: 'subject' is a pointer member, and 'remove'
                      // may change what '*subject' owns
                      // [weavec-safe:owner-path]
}

void remember(Cache &c, std::vector<std::unique_ptr<Item>> &all) {
  c.last = all.back().get();         // a reference; 'all' still owns it
  all.pop_back();
}
int recall(const Cache &c) {
  return c.last->v;   // runs: heap-use-after-free, as today
}
```

`Observer::notify` is the classic: a callback that unregisters itself
while the subject iterates its list. `owner-path` rejects the callback,
which is why `notify_all`'s loop needs no error: nothing a callee can
reach changes `observers` under it.

### Bugs deliberately not caught

- **A dangling reference** (a raw pointer member, a pointer in a
  container): stops when it is used.
- **A shared object changed by another holder.** A borrow into what a
  `std::shared_ptr<T>` points to, kept across a call that reaches the same
  object through another `shared_ptr` and changes what it owns (**D3**,
  §8).
- **A callable that hides its captures.** A `std::function` a safe
  function receives as a parameter may hold borrows of its caller's
  locals; a callee that copies it into a member keeps them (§7). The
  direct case, a lambda that captures by reference converted and stored
  or returned by the function that wrote it, is `escape`.
- **Out-of-range indices and iterators, a null `unique_ptr`, an empty
  `optional`.** Run-time checks, as in Rust.
- **Arguments the standard library forbids.** `v.assign(v.begin() + 1,
  v.end())` passes a container iterators into itself where the standard
  says it must not; `call-alias` trusts library calls with plain borrows
  (§9).
- **Globals.** What a function reads below a global that is not `const`
  is a reference (RFC 0043 §9).
- **Data races**, and a thread or a signal handler that changes an owner
  under a running function (X-A3).
- **Leaks through cycles** of `std::shared_ptr`.
- **What code that is not safe does** against its declarations (X-A1).

### Accepted rejections

A safe function rejects correct C++ that:

- allocates with `new`, releases with `delete`, or manages a raw buffer:
  the implementation of a container or a smart pointer;
- walks an array with a pointer, or indexes one (`p[i]`, `p + n`,
  `++p`); `std::span` and `std::array` replace it;
- converts with `reinterpret_cast`, `const_cast`, a C-style cast between
  pointer types, or `static_cast` down a hierarchy; uses a union;
- keeps two borrows into one container when taking the second is not an
  access (`auto &a = m[k1]; auto &b = m[k2];` on a map whose `operator[]`
  inserts), or keeps any borrow across a call that takes the container as
  non-`const`, even when the call changes nothing;
- stores a `std::string_view`, a `std::span`, an iterator or a reference
  member in an object that outlives a statement and is not itself a view:
  a class with a view of its own string, an LRU cache of `std::list`
  iterators (both are indices or pointers in safe C++, as in Rust);
- changes what an object owns through a raw pointer member (`parent_->
  children_.push_back(…)`, `lexer_->next()`);
- passes a function of the project a container it may change and a
  reference into it (`add(v, v[0])`, `call-alias`);
- uses a moved-from object for its "valid but unspecified" value
  (`s.clear()` after `std::move(s)` is an assignment's worth of reuse and
  is rejected; `s = {}` is accepted);
- is a coroutine, or uses a construct the checker does not model
  (`not-modeled`).

Each is rewritten (the guide gives the accepted form, §13) or kept in a
`WEAVEC_GUARDED` function or class that the audit lists. Gate G3 bounds
the price on correct code; S0 measures it on paper first.

### Assumptions

| | Assumption |
| --- | --- |
| **X-A1** | Code that is not safe (guarded and unsafe functions, system headers, code WeaveC did not compile) honors the declarations safe code sees: a `const` member function or parameter changes nothing an object owns; a result of borrow type points into one of the arguments the caller is told it may (§9), or into something that outlives them. |
| **X-A2** | The arguments with which code that is not safe calls a safe function do not overlap in the way `call-alias` forbids, and a view it passes outlives the call. |
| **X-A3** | No other thread, and no signal handler, destroys, moves or changes an owner while a safe function uses it or a borrow into it. |
| **X-A4** | **(D4)** The standard library behaves as the standard specifies: a `const` member function invalidates nothing, a member function with a `const` overload of the same parameters only gives access (§6), and the types Clang marks as owners and pointers are. |
| **X-A5** | A moved-from object of a library type may be assigned and destroyed. |

## Detailed design

### 1. Safe C++ units

- `weavec-c++ -fweavec-safe` makes every function **defined in the unit's
  own files** safe: the main file and every header that is not a system
  header. That includes member functions defined in a class, lambdas, and
  each instantiation of a template defined in those files.
- Functions from **system headers** (the standard library, the SDK,
  anything found through `-isystem`) are not safe. They are compiled and
  guarded as today, and trusted for their declarations (X-A1, X-A4). A
  project's third-party headers belong behind `-isystem`.
- `WEAVEC_SAFE` on a function definition, or on a class, makes that
  function, or every member function the class defines, safe in a unit
  without the flag. `WEAVEC_GUARDED`, on the same two, takes them out in a
  unit with it. `WEAVEC_UNSAFE` is unchanged.
- A header's inline functions are checked by each safe unit that includes
  it, and not by the others: a project converts a file at a time.
- C++17 or later. `-fweavec-safe-checks=bounds|full` is accepted and has
  no effect in a C++ unit, where a safe function keeps every guard.
- The rule `cxx` and the code that reports it (`rejectCxx`) are deleted.

### 2. Kinds of types

The rules need to know, of a type, whether its objects own, borrow, both
or neither. Four facts, each structural or read from what Clang already
attaches:

- A type **holds owners** if it is not trivially destructible: a class
  with a destructor to run, or one with such a member or base. `int`, a
  struct of scalars and `std::array<int, 4>` hold none; `std::string`,
  `std::vector<T>`, `std::unique_ptr<T>` and any class with one as a
  member do. Changing an object can end a borrow only if its type holds
  owners.
- A type is a **view** if it is a pointer or a reference when it is the
  type of a local, parameter or result; a class Clang marks
  `[[gsl::Pointer]]` (it marks `std::basic_string_view`, `std::span`,
  `std::reference_wrapper` and the library's iterators itself, the
  attributes `-Wdangling-gsl` reads); a closure type with a capture by
  reference, of `this`, or of a view by copy; a class with a member of
  reference type, of view type, or a pointer member declared
  `WEAVEC_BORROWED`; a class with a view as a base; an array of views; or
  a specialization of a library template with a view among its type
  arguments (`std::vector<std::string_view>`, `std::optional<Token>`,
  `std::pair<It, bool>`, `std::tuple<int &>`, `std::unique_ptr<Parser>`); a
  reference type among the arguments counts as a view.
- A raw pointer **member** (`T *last;`) is not a view: it is a reference,
  and does not make its class one. That is the choice a class makes
  between the two: `Lexer &lexer_` is checked when the program compiles
  and ties the object to a scope; `Lexer *lexer_` may live anywhere and is
  checked when it is used.
- A library class is **erased** if it can hold an object of a type its
  own type does not name: `std::function`, `std::move_only_function`,
  `std::any`, `std::packaged_task`, `std::thread` and `std::jthread`. The
  library table lists them (`class std::function : erased;`, §11). An
  erased object made from a view is a view (§7).

A type's kind is computed once per unit and cached; S0 confirms the list
of what Clang marks under libc++ and libstdc++.

### 3. The rules

Each has a stable identifier, a fix note, tests under `test/Safe/` and a
page `--weavec-explain` prints. Rules shared with C keep their identifier
and gain a C++ section on their page.

| Id | Rule in a safe C++ function |
| --- | --- |
| `raw-owner` | No `new` or `delete` expression, no explicit destructor call, no call of a function the library table marks as returning or releasing an owner (`malloc`, `free`, `strdup`), no `release()` of a smart pointer, no smart pointer made from a raw pointer. Blocks are made by `std::make_unique`, `std::make_shared` and containers. |
| `index-single` | A raw pointer points to one object: no subscript on a pointer, no arithmetic, increment, decrement or ordering on pointers. An array is indexed in place (its bounds are checked as today) and converts only to a `std::span`. |
| `pointer-cast` | No `reinterpret_cast`, no `const_cast`, no C-style or functional cast that is one of them, no `static_cast` from `void *` or from a base to a derived class (`dynamic_cast` is the checked form), no `std::launder`, no `std::bit_cast` to or from a pointer. |
| `pointer-bytes` | The bytes of an object that is not trivially copyable are not read or written: no `memcpy`, `memset`, `memmove` or `memcmp` on one. |
| `union-pointer` | No union with a member that is a pointer, a view or a type that holds owners; `std::variant` is the checked form. |
| `int-to-pointer`, `variadic`, `no-asm`, `unsafe-block`, `bad-annotation` | As RFC 0041. `bad-annotation` also rejects a C bounds annotation or `WEAVEC_OWNED` in a safe C++ function's declaration. |
| `owner-moved` | An object is not used after it was moved, on any path that reaches the use, except to assign to it or destroy it (§5). |
| `stale-borrow` | A borrow or a view is not used after an owner it points into was destroyed, moved, assigned or changed, by the function or by a call that could (§6). |
| `call-alias` | In one call, an argument through which the callee could change what an object owns does not point to the same object as another borrow argument, nor above or below it, as far as the function knows where both came from (RFC 0043 §5). A call of a standard library function is checked only for callables (§9). |
| `owner-path` | What an object owns is changed only along a path that owns it or borrows it: from a local, a parameter or `this`, through members, owners and borrows. Not through a raw pointer member or element, not through a pointer or reference to `const`, and not below a global read in place (§8). |
| `escape` | A borrow or a view of a local is not returned, not stored outside the function's locals, and not passed for a `WEAVEC_ESCAPES` parameter; a view is not stored in an object that is not one (§7). |
| `not-modeled` | A construct the checker does not model is rejected, not guessed at: a coroutine, `goto` into or out of a scope with owners, a GNU statement expression, `setjmp`, placement `new`. |

`owner-leak`, `owner-source`, `owner-slot` and the typed-memory rules
(`counted-pair`, `call-args`, `string-index`, `string-write`,
`field-address`) are C's and do not apply: destructors and the library's
types do their work.

### 4. No raw memory

**(D1)** The rules of this section are checks of single expressions, done
in `SafeCheck.cpp` beside C's.

Safe C lets a pointer carry a bound (`WEAVEC_COUNTED_BY`) and checks its
arithmetic. Safe C++ does not: a pointer and a count is spelled
`std::span<T>`, whose `operator[]` the hardened library checks (RFC 0039,
`library-assertion`). So a raw pointer in safe C++ is only ever a borrow
of one object or a reference to one, and (X5) is a ban, not a check.

The same reasoning removes the raw owner. A safe C function declares
which pointers own and the checker follows every release. A safe C++
function has `std::unique_ptr`, the containers and destructors, and
needs no release it could get wrong. A class that must manage memory
itself (a custom container, an arena, a wrapper of a C library's handle)
is the implementation of an owner: it is `WEAVEC_GUARDED`, small, listed
by the audit, and safe code uses it as it uses the standard library's.

Calls into C stay possible. A safe function may call `fopen`, `strlen`
or a project's C API with borrows; the library table's guards and the
callee's own guards check what they do, as today. What it may not do is
take a raw owner from one (`raw-owner`): `strdup`'s result goes into a
guarded wrapper.

### 5. Moves

An object of a type that holds owners is **moved** when `std::move(x)`,
or a cast to an rvalue reference, is bound to a parameter taken by value
or by rvalue reference, or initializes or is assigned to another object;
and when it is returned. The checker assumes a parameter that could take
the value did (`m.try_emplace(k, std::move(v))` moves `v`), as Rust does.

- After a move the object may be assigned as a whole or destroyed. Any
  other use is `owner-moved`, with a note at the move. A member function
  called on it is a use, `clear()` included.
- Where paths join, an object moved on one is moved (RFC 0043 §3).
- A member moved out of `*this`, or out of a parameter taken by lvalue
  reference, is assigned again before the function returns, calls
  anything that could see the object, or uses it whole. A parameter taken
  by rvalue reference, and `*this` in a destructor or an `&&`-qualified
  member function, may be left moved-from: the caller treats it as moved.
- `std::move` of a type that holds no owners is a copy and moves nothing.
- A lambda's capture by move (`[p = std::move(p)]`) moves.

### 6. Borrows, and what ends them

A borrow remembers the **owners it points into**, as RFC 0043 §5 has it
for C:

- `&x`, a reference bound to `x`, and `x.member` point at `x` and into
  no owner: they end when `x` is destroyed or moved, and nothing else
  ends them.
- The result of a member function called on an object `o`, when the
  result is a view (`v[0]`, `v.begin()`, `s.c_str()`, `*p`, `p.get()`,
  `opt.value()`, `std::string_view(s)`), points **into** `o`: into what
  `o` holds. So does a borrow made from such a borrow (`v[0].name`,
  `&*it`).
- The result of any other call that is a view points into every argument
  it could have come from (§9).
- A copy of a borrow points where the borrow does. A view points into
  whatever it was made from (§7).

A borrow into an owner `o` **ends** when:

1. **`o` is destroyed, moved or assigned.** The end of a local's scope,
   the end of the full expression for a temporary that was not extended,
   `std::move`, `o = …`, `o.reset()`.
2. **The function changes `o`**: it calls a member function of `o` that
   is not `const` and not an _accessor_, or changes what `o` owns through
   another borrow (RFC 0043 §5, rule 2: the checker does not tell `v[i]`
   from `v[j]`, so changing an element's owners through one borrow ends
   the other borrows into the same root).
3. **The function makes a call that could change `o`**: `o`, or a
   borrow that reaches it, is passed for a parameter of pointer or
   reference type to a type that is not `const` and holds owners (RFC
   0043 §5, rule 3). A call can change nothing through a `const`
   parameter, through a parameter whose type holds no owners
   (`std::swap(a, b)` of two `int &`), or through an argument taken by
   value.

Using a borrow after it ended is `stale-borrow`, with a note at what
ended it. Assigning the local again starts a new borrow.

An **accessor** is a non-`const` member function that has a `const`
overload with the same name and parameters, or a function with an
overload that differs only in taking the same argument as `const`
(`std::get`, `std::begin`). The standard library gives exactly its access
functions such a pair (`operator[]`, `at`, `front`, `back`, `data`,
`begin`, `end`, `find`, `lower_bound`, `value`, `operator*`), because a
function that changes the container could not have the `const` one. An
accessor ends nothing and its result points into the object. The rule
needs no list of names, and it errs the right way: `std::map::operator[]`
and `std::unordered_map::operator[]` insert, have no `const` overload,
and end borrows. In safe code the pair is checked instead of trusted: a
safe non-`const` member function with a `const` twin may change nothing
`*this` owns (`owner-path`).

Two consequences an agent meets in its first hour, both as in Rust:

- `auto &a = v[0]; auto &b = v[1]; a = b;` compiles for a vector of
  `int`, since nothing here can end a borrow. For a vector of
  `std::string`, `a = b` changes what an element owns through `a`, which
  ends `b` (rule 2): using `b` afterwards is `stale-borrow`, because the
  checker does not tell `v[0]` from `v[1]`. The pointer is taken again,
  or the code works by index.
- A range-`for` holds a borrow into its range for the whole loop. A body
  that changes the range ends it, and the error is at the loop's next
  step, with the note at the change (bug 3).

The false warnings of `fp.cpp` do not arise: a reference parameter `v`
points _at_ a vector and into no owner, so `v.push_back(1);
v.push_back(2);` ends nothing that is used.

### 7. Views

A view is a borrow that is also an object, and the rules follow it by
value:

- **Made.** A view made by a constructor, a conversion or a call points
  into what its arguments point into, and into the arguments taken by
  reference: `std::string_view sv = s;` into `s`; `std::span sp(v)` into
  `v`; `Parser p(lexer)`, for a `Parser` with a `Lexer &` member, at
  `lexer`; `auto f = [&] { … };` at every variable it captures;
  `std::vector<Token> t = lex(source);` into what `source` points into. A
  view made from no borrow (`std::string_view sv = "literal";`, an empty
  vector of views) points nowhere and never ends.
- **Widened.** A member function that is not `const`, called on a view
  with arguments that are borrows, may store them: afterwards the view
  also points where they do (`parts.push_back(sv)`; `p.reset(other)`).
  The same holds for a view passed to a call by non-`const` reference
  with other borrows. This is the whole lifetime system: there are no
  lifetime parameters, and a view's borrows are a set the function
  computes.
- **Used.** Using a view after any owner it points into ended is
  `stale-borrow` (bugs 5, 12, 14, 15).
- **Returned.** A function returns a view only of what its parameters
  (and `this`) point into, or of nothing; a view of a local is `escape`
  (bugs 6, 7, 8).
- **Stored.** A view is stored only in a local or in a view. Inside a
  member function of a view class, a view member is assigned only from
  the function's parameters, which the caller accounts for by widening,
  and not from a borrow of `*this` or of one of its members: an object
  that points into itself breaks when it is moved, and is `escape`. A
  view is not stored in a global, a `static` or a `thread_local`, and is
  not converted to a base class that is not a view.
- **Erased.** An object of an erased type (§2) made from a view is a view
  for as long as the function can see that: `std::function<int()> f = [&]
  { return x; };` points at `x`, may be called and passed, and may not be
  returned or stored in a member (bug 8). `std::thread t([&] { … });` is
  a view of what the lambda captures; `t.join()` uses it, and returning
  or storing `t` is `escape`. In a safe function's own
  templates and constructors the same rule is checked inside: a member of
  erased type is not initialized from a view parameter.
- **Received.** A parameter of erased type may hold borrows the function
  cannot see, which last for the call (X-A2). A safe function calls it
  and passes it on; copying or moving it into a member, a global or the
  result needs the parameter declared `WEAVEC_ESCAPES`, and then every
  safe caller passes a callable that is not a view. This closes the
  callback that outlives its captures at safe call sites, and leaves the
  ones that go through code that is not safe to the guards (X6).

`WEAVEC_ESCAPES` on a pointer or reference parameter keeps RFC 0041's
meaning: the callee keeps a reference to the argument. A safe caller
does not pass the address of a local, or a borrow of one, for it
(`escape`); C++ objects are not moved to the heap for the caller as C
locals are.

### 8. References and shared ownership

A raw pointer loaded from a member, an element or a global is a
reference. Storing a borrow into a pointer member or element is allowed,
with the one exception `escape` makes for locals; the rules then stop
following it. Reading through a reference is allowed and guarded.

**(D3)** `owner-path` is what makes (X3) a statement about the whole
call tree and not only about one function: if no safe function changes
what an object owns except along a path that owns or borrows it, then a
callee that was not handed an object cannot change it, and a borrow
survives every call that does not receive its owner as non-`const`. So:

- Through a raw pointer member or element a safe function may read, call
  `const` member functions, and assign members whose types hold no
  owners (`stats_->count++`, `node->prev->next = node->next` for pointer
  members). It may not call a non-`const` member function of a type that
  holds owners, or assign such a member: `parent_->children_.push_back(x)`
  is `owner-path`. The fix is one of four, and the note names them: own
  the object; take it as a parameter of the function that changes it;
  make the member a reference (`Lexer &`), which makes the class a view;
  or share it.
- **`std::shared_ptr<T>`** says the object is shared. `*sp` and `sp->`
  are borrows into `sp`, so the object lives while the function's handle
  does. Through `std::shared_ptr<const T>` nothing is changed and the
  rules are complete. Through `std::shared_ptr<T>` a safe function may
  change what the object owns, and a borrow some other function holds
  into the same object through its own handle is not ended: the one hole
  this RFC opens on purpose. Rust closes it with `RefCell`'s count,
  checked when the program runs; here the guard on the stale borrow's
  next access is that check. _Unresolved question_ 2 asks whether to
  reject it instead.
- `std::weak_ptr<T>::lock()` gives a `shared_ptr`; a `shared_ptr` member
  is an owner of its handle.

### 9. Calls

- **Results.** A call whose result is a view returns a borrow into its
  arguments. Which ones: those marked `[[clang::lifetimebound]]`
  (`WEAVEC_LIFETIMEBOUND`), `this` included, if any is marked; otherwise
  every argument that is a view or is taken by reference, and `this`. A
  result that points into more than it needs to ends earlier than it
  needs to, and marking the declaration fixes it. A safe function is
  checked against the same reading: it returns a view only of the
  parameters its declaration allows.
- **Arguments.** What a call may change is read from its parameters'
  types (§6, rule 3). An indirect or virtual call is judged by the
  declaration it is made through.
- **`call-alias`** is RFC 0043's: `add(v, v[i])` passes a way to change
  `v` beside a borrow into it. Arguments whose types hold no owners are
  never in conflict. A function of a system header is trusted with the
  arguments its specification allows (X-A4): `std::sort(v.begin(),
  v.end())` passes two iterators into one vector and `v.push_back(v[0])`
  a reference into the vector it grows, and the standard requires both to
  work. What stays checked there is a callable: a lambda that changes an
  object it captured by reference, passed beside a borrow into that
  object (`std::for_each(v.begin(), v.end(), [&](int x) {
  v.push_back(x); })`), is `call-alias`. Whether a lambda changes a
  capture is read from its body, which is in the function.
- **Safe and not safe.** A safe callee believes its parameters: its
  borrows last for the call and do not overlap as `call-alias` forbids
  (X-A2). A safe caller believes the declaration it sees (X-A1). A
  function declared without `const` ends its callers' borrows whether or
  not it changes anything; the fix improves the declaration.

### 10. The rest of the language

- **`this`** is a borrow parameter, `const` in a `const` member function.
  A `const` member function changes nothing `*this` owns, `mutable`
  members included: a cache in a `mutable` member that holds owners is
  changed in a `WEAVEC_GUARDED` helper (`owner-path`).
- **Constructors and destructors** are functions. A member initializer
  is an initialization; a destructor's body runs before its members are
  destroyed. A class with a user-written destructor that releases
  something by hand is an owner's implementation (§4).
- **Temporaries** end at the end of their full expression unless a
  reference extends them, as the CFG's destructor elements say.
- **Lambdas** are checked as functions when they are defined, with their
  captures as members of `this`. A lambda that captures by reference is a
  view of what it captures; one that captures by copy or move is an
  owner.
- **Templates** are checked where they are instantiated, each
  instantiation as a function, with one error per source location and a
  note naming the instantiation. A template is never rejected for what
  some other argument might do.
- **Inheritance and virtual calls.** A derived class has its bases'
  kinds. `static_cast` to a derived class is `pointer-cast`.
- **Exceptions.** At a handler, an object is moved if it was moved
  anywhere in the `try` block, and a borrow has ended if anything in the
  block ended it. Destructors that run during unwinding end borrows as
  at a scope's end.
- **Globals and statics.** A `const` global is read freely. Any other is
  read in place as a reference (RFC 0043 §9), and a view is not stored in
  one.
- **Coroutines**, and the constructs of `not-modeled`, are rejected.

### 11. How it is checked

- **Where.** The single-expression rules (§4) go in `SafeCheck.cpp`. The
  flow rules share the engine of `SafeOwnership.cpp`: S1 splits that file
  into the part that is the same for both languages (places, borrows and
  what they are below, the dataflow, joins, branch refinement, the final
  reporting pass) and C's front (owners' states, leaks, slots, `realloc`).
  A new `SafeCxx.cpp` is C++'s front: kinds of types, member calls and
  accessors, moves, views, temporaries, lambdas, handlers.
- **The CFG** is Clang's, built as RFC 0043 amendment 12 builds it, with
  implicit and temporary destructors, the ends of lifetimes, member
  initializers and exceptional edges. S0's prototype confirms it gives
  what §5 to §7 need for range-`for`, temporaries bound to references,
  lambdas and handlers; where it does not, the form is `not-modeled`.
- **Templates.** The checker visits instantiations of function templates
  and of member functions of class templates whose definitions are in the
  unit's own files. It never descends into a system header's bodies.
- **The library table** gains class rows for erased types
  (`class NAME : erased;`). Nothing else about the standard library is
  listed: owners and pointers come from Clang's attributes, accessors
  from overloads, `std::move` and `std::forward` from Clang's builtin
  recognition of them.
- **Clang's lifetime-safety analysis** is not enabled by `-fweavec-safe`
  and its warnings stay Clang's. The test suite runs it over the seeded
  bugs as an oracle (gate G8).
- **Budget. (D5)** About 1,900 lines for `SafeCxx.cpp`, 350 for the
  expression rules, 250 for kinds, the table, the driver, the ledger and
  the audit, and S1's split should give back about 200. The library's
  limit becomes 20,500 (17,487 of 17,500 today); the runtime's stays
  10,500 and is not touched. S0 measures both numbers again.

### 12. Run time, the ledger and the audit

- **Run time.** The guard passes do not read the flag in a C++ unit.
  `GuardSafe` and `GuardTyped` run for C only, as today.
- **The ledger** (version 7): a C++ function's `tier` is `safe`, `guarded`
  or `unsafe`; its rows keep the reasons guarded C++ has. The `fields`
  list covers the classes the unit's own files define, with the kinds
  `owner`, `reference`, `borrowed` (a reference member, a view member, a
  `WEAVEC_BORROWED` pointer), `shared` and `erased`. The summary line
  counts safe C++ functions.
- **The audit** (`weavec-audit` version 3) lists what (X1) to (X5) rest
  on: each guarded or unsafe function and class in a safe unit; the
  reference members; the `shared` members whose pointee is not `const`;
  the `erased` members; each `WEAVEC_ESCAPES` parameter; and each function
  that is not safe and returns a view (the declarations X-A1 trusts).
- **`verify`** has nothing new to monitor, since nothing is removed. A
  temporal report inside a safe C++ function is either through a
  reference, a shared object or a global (as designed), or a hole in a
  rule, or X-A1 to X-A5 broken. Telling them apart is done by hand for
  the seeded bugs (gate G8); a report that says which is _Future work_,
  because it would change the site records.

### 13. The loop: explanations and the guide

- **`--weavec-explain`** pages gain a C++ section for each rule that
  applies, with a rejected example and its rewrite as `cpp rejected` and
  `cpp accepted` blocks that `docs/scripts/check-examples.py` compiles
  with `weavec-c++`. `raw-owner` and `not-modeled` get pages. `weavec-c++
  --weavec-explain all` prints the C++ brief: the kinds, then the rules.
- **A guide**, `docs/pages/guides/safe-cxx.md`, written around what an
  agent ports first, each block compiled: a value class with string and
  vector members; a tree of `std::unique_ptr` children with a parent
  pointer that is read and not written; a tokenizer whose tokens are
  views of the source, and the same with owned strings; a visitor over a
  `std::variant`; a registry keyed by index instead of by pointer; an LRU
  cache (an owning map, an order kept by pointer members); a callback
  taken as a template parameter, as `std::function` for the call, and
  kept with `WEAVEC_ESCAPES`; an object shared by `std::shared_ptr<const
  T>`; a guarded class that wraps a C handle. A second page maps Rust's
  vocabulary to this one (`&mut` and `&`, `Rc<RefCell<T>>`, lifetimes,
  `Option::take`), because an agent that knows one borrow checker learns
  the second from the differences.
- `guarantees.md` gains (X1) to (X6) and X-A1 to X-A5; `cli.md` the flag
  in C++ and the audit's version; `docs/annotations.md` what the macros
  mean on C++ declarations.

### 14. Cleanups

- `rejectCxx`, the rule `cxx`, its page and its tests are deleted.
- `SafeOwnership.cpp` (2,821 lines, one class) is split as §11 says.
- `resources/safe-c/` becomes `resources/safe/`, and the guide's rules
  reference one page for both languages.
- The roadmap's seven "Now" sections become one, with RFCs 0038 to 0043
  moved to _History_ once their statuses are settled; RFC 0041 §12 and
  the documentation's "C++ comes in a later RFC" are removed.

### 15. Stages and gates

- **S0 On paper, and probes.** The rules applied by hand to three files
  of C++ WeaveC did not write: one from a modern project of the corpus,
  one from an older one, and the Rust port of RFC 0043's gate G5
  translated line for line; every rejection recorded by rule. A throwaway
  dataflow for `owner-moved` and rule 1 of §6 over `bugs.cpp`, to confirm
  the CFG. The frozen sets for G2 and G3, the two ports of G4 and the two
  conversions of G5 are chosen. What Clang marks as owners and pointers
  under both standard libraries is listed, and the accessor rule is
  checked against every non-`const` member function of the containers
  (any that has a `const` twin and invalidates becomes a table row).
  Thresholds below are confirmed or changed here.
- **S1 Cleanups** (§14), the flag in C++, `WEAVEC_SAFE` on classes, the
  ledger's version.
- **S2 Expression rules** (§4).
- **S3 Kinds of types and moves** (§2, §5).
- **S4 Borrows**: what each points into, the three ways one ends,
  accessors, `call-alias`, `owner-path`.
- **S5 Views**: made, widened, returned, stored, erased; `escape`.
- **S6 Explanations and the guide**, the audit.
- **S7 Ports, conversions and seeded bugs**, by agents; the gates on
  darwin-arm64, linux-aarch64 and linux-x86_64 (libc++ and libstdc++).

The gates:

- **G1 Rules.** Each rule has accepted and rejected C++ tests, a fix
  note and a page whose examples are checked.
- **G2 Seeded bugs.** The measure of the owner's goal. A frozen set of at
  least 120: the twenty of the probe, and mutations made by a script and
  by an agent asked for plausible mistakes in the guide's blocks and the
  ports (a re-acquired borrow moved above the change that invalidated it;
  a use moved after `std::move`; a `const` removed from a callee; an
  owned string replaced by a view; a view returned or stored; a capture
  by copy made a capture by reference). Of the bugs on owners, borrows
  and views, at least 90% stop the compile with the right rule. Of the
  probe, the 15 this RFC's table promises. Today: none.
- **G3 Correct code.** A frozen set of at least 80 correct functions in
  idiomatic C++17 and C++20, written before the rules were shown to their
  author (the guide's blocks do not count), `fp.cpp` among them. At least
  90% compile unchanged; each rejection is classed by rule and by
  whether Rust would reject its translation.
- **G4 The ports, against Rust.** Two specifications, each ported by an
  agent into safe C++ and by another into Rust, from the same brief and
  tests: RFC 0043's LRU cache unchanged, so that Safe C, safe C++ and
  Rust can be read side by side, and a larger tree-shaped program S0
  picks (a JSON document library or an expression interpreter, 1,500 to
  3,000 lines in the source language, which is neither C++ nor Rust).
  Safe C++: at least 95% of functions safe, tests pass under the guards
  with no report. Rounds to a clean build and wall time at most twice
  Rust's. Recorded: lines, errors by rule, the benchmark against Rust
  and against the same C++ compiled by Clang.
- **G5 Conversions.** Two existing C++ projects from RFC 0039's corpus,
  one modern and one older, converted in place by agents given the guide
  and `--weavec-explain`: tests pass; at least 70% of functions safe in
  the modern one. The older one is recorded, not gated: it measures what
  C with classes costs.
- **G6 Defects found.** Every first-round error of G5 that an agent
  classes as a defect in the original is reproduced under the guards and
  recorded.
- **G7 Cost.** A unit compiled with and without `-fweavec-safe` has the
  same object code. `-fsyntax-only` on each unit of G4 and G5 is at most
  1.15 times as long with the rules as without, 1.30 for the worst unit.
- **G8 Holes.** Each seeded bug that compiles is run under the guards.
  One that reports through something other than a reference, a shared
  object or a global is a hole: fixed, or listed with its reason. A
  seeded bug Clang's lifetime analysis reports and the rules accept is
  listed the same way.
- **G9 No change elsewhere.** Safe C's tests and conversions compile and
  run as on main; a C++ unit without the flag is compiled as on main; the
  corpus gate's results do not change.
- **H1 Hygiene.** The library within 20,500 lines.

## Annotation surface

`weavec.h` becomes version 3.1. No macro is added.

- **`WEAVEC_SAFE` and `WEAVEC_GUARDED`** may be written on a class, where
  they apply to every member function the class defines.
- **`WEAVEC_ESCAPES`** on a C++ parameter: the callee keeps the argument
  (a pointer or reference as a reference; an erased callable as a copy).
- **`WEAVEC_BORROWED`** on a pointer member makes it a borrow and its
  class a view, as a reference member is.
- **`WEAVEC_LIFETIMEBOUND`** is `[[clang::lifetimebound]]`, on a parameter
  or after a member function's declarator for `this`.
- **`WEAVEC_OWNED`, `WEAVEC_DROPS` and the bounds annotations** are C's.
  In a safe C++ function's declaration they are `bad-annotation`; on an
  `extern "C"` declaration a C++ unit includes they are accepted and
  ignored.

## Diagnostics

**Compile-time errors**, in the form of RFC 0041, each pinned by a
`test/Safe/` test:

```
grow.cpp:14:10: error: 'first' points into 'items', which 'push_back' may have changed [weavec-safe:stale-borrow]
grow.cpp:13:9: note: changed here; take 'first' again after this call, or keep an index instead of a reference
grow.cpp:14:10: note: 'weavec-c++ --weavec-explain stale-borrow' explains this rule

view.cpp:6:22: error: 'sv' points into a temporary 'std::string' that is destroyed at the end of this statement [weavec-safe:stale-borrow]
view.cpp:6:22: note: keep the string in a local: 'std::string s = make(n);'

loop.cpp:9:3: error: the loop over 'v' continues after 'push_back' may have changed 'v' [weavec-safe:stale-borrow]
loop.cpp:10:20: note: changed here; loop by index, or collect what to add and append it after the loop

call.cpp:21:14: error: 'n' was taken from 'tree' before the call to 'insert', which may change what 'tree' owns [weavec-safe:stale-borrow]
call.cpp:20:3: note: take 'n' again after the call, or declare the parameter 'const Tree &' if 'insert' changes nothing

moved.cpp:8:10: error: 'it' is used after it was moved [weavec-safe:owner-moved]
moved.cpp:7:17: note: moved here; use it before this call, or assign to it first

tokens.cpp:12:10: error: 'tokens' holds borrows of the local 'source' and is returned [weavec-safe:escape]
tokens.cpp:11:30: note: borrowed here; return the text by value ('std::string' in 'Token'), or take 'source' as a parameter so the caller keeps it

cb.cpp:17:19: error: a lambda that captures 'count' by reference is stored in the member 'on_done_' [weavec-safe:escape]
cb.cpp:17:19: note: capture by copy or move ('[count]', '[p = std::move(p)]'), or keep a 'std::shared_ptr' to what the callback needs

self.cpp:9:5: error: 'text_' would point into 'buffer_', a member of the same object [weavec-safe:escape]
self.cpp:9:5: note: keep an offset and a length instead of a view

make.cpp:4:10: error: 'new' makes a block no object owns [weavec-safe:raw-owner]
make.cpp:4:10: note: 'std::make_unique<Item>()' returns its owner

obs.cpp:19:3: error: 'subject' is a pointer member, and 'remove' may change what '*subject' owns [weavec-safe:owner-path]
obs.cpp:19:3: note: change an object from what owns it or is passed it: take 'Subject &' as a parameter of this function, hold it by 'std::shared_ptr', or keep this function WEAVEC_GUARDED

add.cpp:5:3: error: 'v[0]' points into 'v', which 'add' may change through its first argument [weavec-safe:call-alias]
add.cpp:5:3: note: copy the element first: 'auto x = v[0]; add(v, x);', or make the other parameter 'const'

walk.cpp:7:12: error: arithmetic on the pointer 'p' [weavec-safe:index-single]
walk.cpp:7:12: note: take a 'std::span<const int>' and index it

coro.cpp:3:1: error: a coroutine is not checked [weavec-safe:not-modeled]
coro.cpp:3:1: note: keep this function WEAVEC_GUARDED
```

**Run-time report kinds.** None is new and none changes.

## Drawbacks

- **A subset of C++ (D1).** Much C++ that exists does not fit: raw
  arrays, intrusive lists, classes that hold references to their
  services, anything written before C++11. Converting it is a rewrite of
  the kind this project exists to make unnecessary, and G5's older
  project is there to say how large.
- **No speed (D2).** Safe C++ costs 1.90 times the reference (RFC 0039's
  five workloads), where RFC 0043's Rust port cost 1.32, its Safe C port
  1.81, and Safe C's conversions 1.45. Until the rules are
  spent, the case for safe C++ over Rust is the loop and the conversion
  in place, not the benchmark.
- **Stricter than C++ programmers expect.** A borrow ends at any
  non-`const` use of its container, including `insert` into a `std::map`,
  which invalidates nothing. Rust is as strict; C++ code is not written
  that way, and `const` will have to spread through signatures.
- **Holes on purpose (D3).** Shared objects and callables received
  through erased types are not followed. They are where "it compiles"
  stops meaning what it means in Rust.
- **Trust in a large library (D4).** The accessor rule and Clang's
  attributes are assumptions about two implementations of the standard
  library that neither promises to keep.
- **C++ is large.** Every form the checker meets is modeled, rejected or
  wrong. `not-modeled` is the honest second choice, and it will be hit.
- **Checked per instantiation.** An error inside a project's template
  appears when some unit instantiates it, with a note that names the
  arguments, and compile time grows with instantiations.
- **Two dialects.** Safe C and safe C++ share kinds and rule names but
  not annotations: C declares owners and bounds, C++ uses its types. A
  project in both learns both.
- **About 2,300 more library lines** and a limit raised by 3,000, a
  fourth budget increase in four RFCs (D5).

## Alternatives

- **Go on with C**: generics for `void *`, ownership by outcome and by
  flag, init parameters, the cost items of RFC 0042. Each is real and
  measured, and together they make Safe C a better C. They do not make C
  the language someone ports a Python service or a C++ engine into, and
  each adds a WeaveC-only construct to a language whose advantage was
  that models already know it.
- **Spend the rules first** (release without the quarantine, loop ranges
  for comparisons): Safe C from 1.45 toward 1.2. It moves a number that
  is already near Rust's on the port (1.37 times), for a language the
  rewrite is not going to. It should follow this RFC for both languages.
- **Turn on Clang's lifetime safety as errors.** Measured above: about
  half the probe, false errors on two `push_back` calls, no moves, no
  calls, no statement of what holds, and nothing WeaveC can fix without
  carrying a patched Clang. Kept as an oracle.
- **Safe C++ as a new language** (Circle's Safe C++, P3390: `safe`
  functions, `^` borrows, lifetime parameters, a second standard
  library). It is Rust's model in full and it is sound without a runtime.
  No existing C++ compiles as it, no model has seen it, and it needs the
  second standard library before the first program. WeaveC's position is
  the opposite: the standard library people have, the guards for what the
  rules leave, no new syntax.
- **Profiles** (the C++ committee's direction: bounds, type and lifetime
  profiles as opt-in restrictions). The same shape as this RFC without
  an implementation or a runtime under it. §4 is close to the bounds and
  type profiles; if they are standardized the rule names can follow.
- **Run time only**: say guarded C++ is the product (as Fil-C is for
  C and C++, with capabilities and a collector). It is memory safe, and
  the measurements say it gives an agent nothing when it compiles.
- **Make the rules load-bearing now**: drop guards in safe C++ where the
  library's hardening already checked the index, which is where C++'s
  1.90 comes from. That is the next RFC. Doing it in the same change
  would put the first C++ borrow rules under the program's safety on the
  day they are written, which RFCs 0030 to 0034 did with an analysis and
  the assessment found wrong proofs.
- **Lifetime parameters for views**, instead of a set per local. It
  would let a view be stored in the heap and returned through layers,
  and it is the part of Rust agents spend their rounds on. If G3 and G4
  show what one named lifetime would buy, it is added then.
- **Reject changes through `shared_ptr` too**, with a checked cell type
  for shared state. Sound, and it needs a WeaveC library type in every
  program that shares anything. See _Unresolved question_ 2.
- **A list of invalidating functions** in the library table instead of
  the accessor rule. More precise (`std::map::insert` would end nothing),
  and a table of a few hundred rows over two standard libraries to keep
  true. The rule is one sentence and errs toward ending borrows.
- **Data races next.** Rust's other guarantee, and nothing here gives it.
  The kinds are a start (a view is not sent to a thread that outlives its
  scope), and a real answer needs `Send` and `Sync` for C++ types: its
  own RFC.

## Prior art

- **Rust**: moves, borrows that end when the owner is used mutably,
  two-phase and non-lexical borrows, `'static` bounds on what a thread or
  a stored closure may capture, `Rc<RefCell<T>>` for shared state checked
  at run time. Taken: the first two and the capture rule. Not taken:
  lifetime parameters, the difference between shared and unique borrows
  of plain data (`int &` twice is fine here), `Send` and `Sync`.
- **The C++ Core Guidelines' lifetime profile** (Sutter, 2015 to 2019;
  the `-Wlifetime` prototypes in Clang and MSVC): Owners and Pointers,
  a Pointer invalidated by a non-`const` use of its Owner, `gsl::Owner`
  and `gsl::Pointer`. This RFC's §2 and §6 are that design. It warned,
  had no account of memory, and stalled on false positives; here
  references are given to the runtime instead of to the analysis, and
  the accessor rule replaces its lists.
- **Clang's lifetime-safety analysis** (2025 onward) and
  `[[clang::lifetimebound]]`, `[[clang::lifetime_capture_by]]`: measured
  in _Motivation_. Its structure is `SafeOwnership.cpp`'s; its
  annotations are read where they exist.
- **Clang's Safe Buffers** (`-Wunsafe-buffer-usage` with a hardened
  libc++): pointer arithmetic banned in favor of `std::span`, with the
  library checking the index. §4's `index-single` is that model, and RFC
  0039 already turns the library's checks into reports.
- **Circle's Safe C++** (Baxter, P3390) and **Hylo** / **Swift**
  (`borrowing`, `consuming`, the law of exclusivity; `~Escapable` types
  and lifetime dependence for `Span`): two ways to add borrows to a
  language with value types. Swift's non-escapable types are this RFC's
  views: values that hold a borrow, confined by where they may be
  stored, with a dependence computed from the call that made them.
- **Chromium's `raw_ptr<T>`** (MiraclePtr): pointer members checked at
  run time by quarantining what they point to, while locals and
  parameters are left to review and static checks. The split between
  references and borrows, arrived at from the other side.
- **`clang-tidy`'s `bugprone-use-after-move`** and Clang's `-Wconsumed`:
  move tracking as a lint; §5 is the same analysis as a rule.
- **Fil-C**, **CHERI C++**, AddressSanitizer: memory-safe C++ at run
  time. What `weavec-c++` already is, and what stays under the rules.

## Unresolved questions

1. **Does idiomatic C++ fit?** G3's 90% is a guess. The likeliest
   failures: borrows across non-`const` calls that change nothing
   (node-based containers, getters without `const`), classes with
   reference members kept in containers, and `std::function` members.
   S0's paper review decides whether the rules or the thresholds move.
2. **Shared objects (D3).** Allowing changes through `std::shared_ptr<T>`
   leaves a hole; rejecting them needs a checked cell. How often do the
   ports share mutable state at all, and how many seeded bugs fall in the
   hole?
3. **Raw pointer members (D3).** Is `owner-path` on them affordable in
   conversions of existing code, where a non-owning pointer to a service
   object is everywhere? G5 measures it; the alternative is to treat a
   pointer member as a borrow of unknown origin and lose (X3) across
   calls.
4. **Is the accessor rule true** of every container in libc++ and
   libstdc++, `std::vector<bool>` and the adaptors included? S0 checks
   each; exceptions become table rows.
5. **The default for results** (a view result points into every borrow
   argument). How often is it too wide, and should the error offer
   `WEAVEC_LIFETIMEBOUND` as a fix-it?
6. **Exceptions.** Whether Clang's CFG gives the exceptional edges §10
   needs, or the handler rule is implemented from the `try` block's
   statements directly.
7. **Templates.** Errors per instantiation can repeat and can appear
   late. Is checking a template's non-dependent part at its definition
   worth the second code path?
8. **Erased types.** Is the table's list enough, and is `WEAVEC_ESCAPES`
   on a callable parameter something agents write unprompted?
9. **Threads.** `std::thread` as a view of its lambda's captures catches
   the thread stored or returned past a local; `detach()` would need a
   table row. Is either worth stating as a guarantee before data races
   are addressed at all?
10. **Mixed projects.** A C++ unit that defines `extern "C"` functions
    for safe C callers: which side's annotations describe the boundary?
11. **Compile time** on template-heavy units, where instantiations
    outnumber functions by an order of magnitude.
12. **Null.** `owner-moved` could also reject `*p` on a `unique_ptr` that
    a test showed null, as RFC 0043's states already track. Worth its
    false rejections?

## Future work

- **Typed C++**: spend the rules. A safe function's accesses through
  borrows need no guard once the rules are trusted, and an index the
  hardened library checked needs no second check; references keep a test
  when they are loaded, as (O4) of RFC 0043. That is where 1.90 goes
  toward Safe C's 1.45, and with RFC 0043's _Future work_ (release
  without the quarantine) toward Rust's.
- **Shared state checked at run time** (a cell with a borrow count), if
  question 2 says the hole matters.
- **Data races**: sendable and shareable kinds for C++ types.
- **Null states** for smart pointers and `std::optional`.
- **Safe C and safe C++ at one boundary**: `std::span` for a counted
  pointer, `std::unique_ptr` with a deleter for a `WEAVEC_OWNED` result.
- **Coroutines**, where a borrow across a suspension is the bug.
- **Fix-its** for the notes that name the rewrite, and a mode that
  applies them.
- **Reports that say whether a temporal error in a safe function went
  through a reference**, so that a hole in a rule announces itself.
- **One named lifetime** for views that must be stored, if the ports ask
  for it.

## Implementation amendments

These record where the implementation departs from the design above: from
building it, from the frozen sets of gates G2 and G3 (written by agents
that had not seen the rules), and from what the port agents reported.
They override the body.

1. **Three files, and no split of `SafeOwnership.cpp` (§11, §14).** Safe
   C's dataflow follows named owner places through null, holds and moved;
   Safe C++'s follows the loans of view locals. They share the shape of
   the solver and the report path, not code, and a common engine would
   have been an abstraction over two users with little in common. The
   C++ checker is `SafeCxx.cpp` (the unit's safe functions, the expression
   rules, the ledger's lists), `SafeCxxFlow.cpp` (the dataflow) and
   `SafeCxxKinds.h` (kinds of types); `SafeOwnership.cpp` is unchanged.
2. **Loans have a depth (§6).** A loan names a root (a local, a parameter,
   `this`, a temporary; with the member it went through) and a depth: 0
   at the root, 1 into what it holds, more below an element. A change of
   an object ends the loans deeper than the object's own, not those
   beside it: a loop that edits each element (`item.tags.push_back(x)`)
   keeps its iterator, and a pointer into the element's own buffer ends.
   `a = b` for two `std::string &` into one vector ends neither (the body
   said it ended `b`). `call-alias` is the same comparison: a borrow
   deeper than what the callee may change, or the same object twice.
3. **What a view was handed (§7).** Where a `std::string_view` parameter
   points, where a view member of `*this` points, and what a reference
   member names are roots of their own, which nothing the function does
   ends: only the caller knows them.
4. **Results (§9).** A member function's view result points into its
   object and where the views passed by value point; not into its
   reference arguments (`m.find(std::string("k"))` would tie the iterator
   to the temporary key). A function that is not a member: where the
   views passed by value point, or, when there is none, into its
   reference arguments; a reference or pointer result only into arguments
   of a related type. A library accessor that is not a member
   (`std::get`, `std::begin`: an overload takes the same argument as
   `const`) returns what is inside its argument and ends nothing.
   Through a view that points elsewhere (an iterator, a
   `std::string_view`), a result points where the view points; through a
   view that only holds views (`std::optional<std::string_view>`), it
   does too unless it names one of them (`*optional`).
5. **A class keeps only what its members could (§7).** A view made by a
   constructor, or returned as a class, takes a borrow of an argument
   only if a member could refer to it: a reference or `WEAVEC_BORROWED`
   member of a related type, or a member that is a library view or an
   erased type. `Logger(const std::string &prefix, Sink &sink)` with a
   `std::string` and a `Sink &` member borrows the sink and copies the
   prefix. The fixed twins of the seeded bugs showed the body's rule
   (every reference argument) rejecting the copy.
6. **Callables that are kept (§7, "Received").** The body asked for
   `WEAVEC_ESCAPES` on every parameter whose callable is kept. Built
   first, it rejected five of the ninety correct files. As built: a
   parameter of an erased type **taken by value** is one the callee
   keeps, and one taken by reference to `const`, or a template parameter,
   is called and not kept; a callee that keeps a callable it took by
   reference is `escape`, and `WEAVEC_ESCAPES` still says "kept" where
   the type does not. A safe caller does not pass a callable that
   captures a local by reference where it is kept, unless the keeper is
   a local of the same function (the object of the call, or the local
   being constructed), which then holds the borrows as a view does and is
   not used, returned or outlived past them. A callable returned as an
   erased type owns what it captured (the callee is checked; the caller
   takes no borrow from the call). `std::future` and `std::shared_future`
   are erased rows too, and a library template with an erased argument
   (a vector of callbacks) is erased.
7. **Lent parameters (§7).** Outside its constructors, a class does not
   assign a view member from a parameter it took by reference to an
   owner (`void set(const std::string &n) { name_ = n; }` with a
   `std::string_view name_`): it takes a view, which callers can see. A
   callable that is kept does not capture a `const T &` parameter, which
   may be a temporary.
8. **Moves (§5).** The library table says which member functions reset
   their object whatever it held (`class std::vector : resets(clear,
   assign);`, `reset` of the smart pointers and `std::optional`): after
   one, a moved object is usable again, as after an assignment. Three of
   the ninety correct files move a string and `clear()` it. An element
   moved out of a container (`std::move(v[i])`) is not followed.
9. **Accessors (§6).** A member function, or an instantiation of a member
   template (`find(const K &)`), with a `const` twin. In a safe function
   the pair is checked: one that is `const` or has a `const` twin changes
   nothing `*this` owns. yaml-cpp has a counterexample in code that is
   not safe (a non-`const` `get` that inserts beside a `const` one that
   does not), which X-A1 covers and question 4 asked about.
10. **The expression rules (§4).** `index-single` allows the difference
    and the comparison of two pointers, arithmetic on a pointer whose
    type is a library class's member typedef (an iterator that happens to
    be a pointer), the decay of an array of constant characters and of
    a `va_list` where that is an array (x86-64; `variadic` is its rule),
    and `main`'s `argv`; it rejects a library view made from a pointer and a
    size. `raw-owner` knows `release()` by structure (a library owner's
    member function that is not `const`, has no `const` twin and returns
    a raw pointer) and a smart pointer made from a raw one by its
    constructor. `std::bit_cast` of a pointer is not rejected (it is a
    library function's body). A rule's construct inside a library macro
    is reported where the macro is written only for `variadic` and
    `unsafe-block`.
11. **Globals (§10).** The library's own (`std::cout`) are the library's.
    A global of the program is not changed in place (`owner-path`); it is
    assigned as a whole, so it can be moved into a local and back. A view
    is not stored in one.
12. **Exceptions (§10).** Clang's exceptional edges are enough: a handler
    is reached from each call of its `try` block that may throw. An exit
    by exception does not run the check for members left moved.
13. **Shared pointers (§8)** are known by structure: a library class that
    gives `->`, can be copied, and is neither one of Clang's owners nor a
    view. A borrow into what a local `shared_ptr` points to may be
    returned.
14. **The ledger and the audit (§12).** As §12, except: a `shared_ptr` to
    `const` is listed as `owner`; the audit does not list
    `WEAVEC_ESCAPES` parameters or the functions that are not safe and
    return a view; and it does not list the guards of safe C++ functions,
    which are all kept.
15. **Explanations (§13).** `safe-cxx` is a page of its own in the list
    `--weavec-explain` prints. The rules' directory is `resources/safe/`.
16. **Not built.** A view converted to a base class that is not a view; a
    report line that says whether a temporal error went through a
    reference; fix-its. `not-modeled` covers coroutines, `goto`,
    statement expressions and functions that return twice; placement
    `new` is `raw-owner`.
17. **What the sets added to (X6).** Two classes compile that the body's
    list does not name: a callable that captures `this`, or an object a
    parameter refers to, and is kept by an object that outlives it (two
    of the seeded bugs); and an element moved out of a container and used
    (one).
18. **Budget (H1, D5).** The library's limit is 20,750 lines (20,694
    used), not the 20,500 the body estimated: the flow file is 2,003
    lines where 1,900 were planned for all of the checker, the unit and
    the expression rules 717, the kinds 418, and nothing was given back
    by a split (amendment 1).

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset, a debug
build of the compiler) on 2026-11-13, and checked on linux-aarch64 with
libstdc++ (gate G9). The sets and the ports were made by
Claude subagents (the session's model) that were told not to read the
repository; the two that wrote the sets had no description of the rules
(one reports that the project's notes for agents, which then described
Safe C only, were in its context). The port agents used a copy of the
compiler frozen at their start; the compiler changed after them
(amendments 4 to 11), and every number below is the final compiler's.
The files are in `build/rfc44/` (not committed): `probe/`, `g2/`, `g3/`,
`g4/`, `run-g.py`, `gates-final.txt`.

| Gate | Result |
| --- | --- |
| G1 Rules | **Pass.** `test/Safe/cxx-borrows.cpp`, `cxx-views.cpp`, `cxx-expressions.cpp` and `cxx-units.cpp` pin 104 errors beside their accepted forms; `test/Driver/safe-cxx-units.cpp` the ledger, the audit and a reference stopped at run time; `resources/safe/rules.md` has 38 C++ examples and the guide 9 blocks, all compiled by `docs/scripts/check-examples.py`. |
| G2 Seeded bugs | **Not met as written; 82 of 110.** The frozen set is 110 pairs written by an agent: a program with one temporal bug it thought plausible, weighted as it thought they occur, each confirmed by AddressSanitizer, and its fixed twin. Counting a bug only when it gets an error its fixed twin does not: **82 (75%) stop the compile for the bug** (`stale-borrow` 44, `escape` 25, `owner-moved` 8, others 5). **14 more stop the compile, and so do their fixed twins** (the fix is outside the subset too: self-referential views, raw `new`, pointer arithmetic, a change through a pointer member). **14 compile**: 8 through a raw pointer kept in memory (a reference, by design), 2 by re-entrancy through a stored callable, 2 by a callable that captures `this`, 1 through a `shared_ptr` made inside `emplace_back`, 1 by an element moved out of a vector. By the agent's classes: container invalidation 18 of 24, dangling view 16 of 20, return of a local 9 of 10, escaping capture 12 of 14, use after move 9 of 10, stale pointer from an owner 10 of 12, member bound to a temporary 5 of 6, self-referential 2 of 3, raw `new`/`delete` 0 of 3 (all three stop, as their twins do), dangling back pointer 1 of 8. The gate's "bugs on owners, borrows and views" was not defined before the set existed, so no share of a subset is claimed. Of the twenty of the probe, 16 stop the compile (15 promised; bug 11 is an `escape`). Before this RFC: none. |
| G3 Correct code | **Pass: 87 of 90 (96.7%)** correct files, written by an agent as it would port an application, compile unchanged. The three rejected: a queue with a raw tail pointer into its own list of `unique_ptr`s, a cache that keeps `std::list` iterators in a map beside the list, and an observer list called through raw pointers; none has a safe Rust translation of the same shape. `fp.cpp` compiles. **A harder measure the gate did not ask for:** of the 110 fixed twins of G2, 21 (19%) are rejected: 8 use raw memory or pointer arithmetic (D1), 4 change an object through a pointer member (D3), 3 keep a view into the object's own storage, 4 hold a borrow across a call that takes its owner as non-`const` and changes nothing, 1 leaves a parameter moved, 1 passes a literal for a `const std::string &` beside the reference it returns. |
| G4 The ports | **Pass for what was gated.** Two specifications, each to a safe C++ agent and a Rust agent started together. **The LRU cache of RFC 0043:** safe C++ 22 of 22 functions safe, no rule's error in any round (2 rounds, both for one misplaced annotation), tests pass under the guards with no report, 2 min 18 s, 289 lines; Rust 1 round, 1 min 37 s, 310 lines; both chose links by index. **Python's `json` module** (898 lines of Python; 254 test vectors made by Python): safe C++ 73 of 73 functions safe, 3 rounds to a clean build (16 ordinary C++ errors, then 5 of the rules', then none), 9 min 7 s, 935 lines; Rust 1 round, 7 min 29 s, 1,073 lines. Both pass 645 of 646 checks (the last is a flaw of the checker script). The five rule errors were all false and are fixed (amendments 4, 9, 10, 11); both libraries compile clean with the final compiler. Wall time is 1.4 and 1.2 times Rust's. **Benchmarks** (user and system time, best of five; the LRU's 20 runs): LRU safe C++ 1.61 s, the same C++ by Clang 1.11 s, Rust 1.26 s (1.45 times the reference, 1.28 times Rust); `json` 1.35 s, 0.44 s, 0.42 s (3.1 times the reference and 3.2 times Rust: the benchmark allocates, and safe C++ is guarded C++). |
| G5 Conversions | **Not run.** The checker was run over yaml-cpp, tinyxml2, pugixml and a file of fmt without a crash; yaml-cpp's 30 files give 250 errors, most `owner-path` through its pointer members and `index-single`. No agent converted a project. |
| G6 Defects found | None claimed: no conversion was made. |
| G7 Cost | **Pass.** A unit's object file is byte for byte the same with and without `-fweavec-safe` (`test/Driver/safe-cxx-units.cpp`). `-fsyntax-only` with the rules takes 1.02 times as long on the `json` port's library, 1.03 on the LRU's, and no measurable time more on a yaml-cpp file and a file of G3. |
| G8 Holes | **Pass for the seeded bugs.** The 28 that compile, or stop only as their twin does, were built guarded and run: all 28 stop with a report (25 `heap-use-after-free`, and a `double-free`, a `stack-use-after-scope` and a `null-dereference`). Clang's lifetime analysis was not run over the set. |
| G9 No change elsewhere | **Pass on darwin-arm64 and linux-aarch64.** The `dev` preset's tests pass on darwin (48 of 48: the lit tests, the cases, the runtime, the unit tests) and the `ci-release` preset's on linux-aarch64 in Docker (48 of 48, with libstdc++); Safe C's tests are unchanged but for the removed rule `cxx`; the ledger's version is 7 and the audit's 3. On linux-aarch64 the probe, G2, G3, the ports' sources and the documentation's examples give the numbers above exactly (16 of 20; 82 of 110 with 21 fixed twins rejected; 87 of 90; no error). The corpus gate and linux-x86_64 were not run for this RFC. |
| H1 Hygiene | **Pass with amendment 18**: library 20,694 of 20,750 lines, runtime 9,976 of 10,500. |

What the numbers say about the owner's goal: an agent that had never
seen the dialect wrote two libraries with every function safe, from a
guide and the compiler's explanations, in 1.2 to 1.4 times the Rust
agent's time, and the rules stopped three quarters of an independent set
of temporal bugs when they compiled. What they do not yet say: that
existing C++ converts (G5), and that safe C++ is fast where a program
allocates (D2).
