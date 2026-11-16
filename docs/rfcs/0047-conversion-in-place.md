# RFC 0047: Conversion in place

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-15
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Carries out what
  [RFC 0044](0044-safe-cxx.md) promised in its _Motivation_ ("safe C++ is
  not a rewrite at all: it is a conversion in place") and never measured
  (its gate G5, "Not run"), and its _Future work_ item "Safe C and safe
  C++ at one boundary". Supersedes RFC 0044 §4 for pointers with a bound
  (its decision D1 stays for raw owners), the C++ part of `index-single`
  and of `bad-annotation`, and its decision (D3) with the paragraph of
  its §8 that lets any object be changed through a `std::shared_ptr`.
  Corrects (Y6) of
  [RFC 0045](0045-type-stable-memory.md). Extends RFC 0041's bounds and
  RFC 0042's typed memory to C++, `owner-path` and `stale-borrow`, the
  library table's class rows, the enforcement ledger and the audit. Adds
  two annotations, no rule and no report kind. Changes nothing in the
  runtime.

This RFC was written at the request of the project owner on 2026-11-15,
who asked a seventh time for the single change that would do most to
make an AI rewrite into C or C++ as attractive as an AI rewrite into
Rust, to be implemented next, with breaking changes and the removal of
old code welcome. The measurements in _Motivation_ were taken on main at
`3066dd72` (RFC 0046 merged), darwin-arm64, the `dev` build. The scripts
and their output are in `build/rfc47/` (not committed): `survey/`
(`run.sh`, `uniq.py`, `yield.py`, one log per project) and `probe/` (the
programs quoted below). Nothing of the design has been built or
prototyped; stage S0 is where it meets code.

The owner accepted the design as drafted on 2026-11-15, before any of it
was built, and with it the six decisions that were theirs to make. They
are marked **(D1)** to **(D6)** where they are argued, and collected here.
The thresholds of §10 and the _Unresolved questions_ are still open, and
S0 is where they are settled; question 1 may reopen (D2).

- **(D1) A raw pointer with a bound is part of Safe C++.** A safe C++
  function may index and step a pointer whose declaration, array or
  allocation gives it a bound, exactly as a safe C function may, and the
  access is compared with that bound when it runs. (X5) of RFC 0044 stops
  being a ban and becomes a check. Safe C's bounds annotations mean the
  same in both languages.
- **(D2) An object is changed through a pointer kept in memory only if
  its class says so.** `WEAVEC_ALIASED` on a class allows what
  `owner-path` rejects today, and the same declaration is now needed to
  change an object through a `std::shared_ptr`, which no rule asked for
  before. An aliased class may not own storage that changes type in
  place. In exchange, "no stale borrow" (X3) no longer covers, across a
  call, what an aliased object owns: that is found when the checked build
  runs, as it is for `std::shared_ptr` today.
- **(D3) Raw owners stay out of Safe C++.** `new` is allowed only as the
  argument that a smart pointer takes at once. `WEAVEC_OWNED` remains
  C's. A class that manages memory by hand stays `WEAVEC_GUARDED`.
- **(D4) The file is the unit of conversion.** A marker in a source or
  header file makes the functions that file defines safe, whoever
  includes it; a survey mode counts the errors of every function that is
  not safe yet without failing the build, and the audit prints them as a
  worklist.
- **(D5) The comparison with Rust is a conversion against a rewrite.**
  The gate that decides this RFC gives one existing C++ project to two
  agents: one converts it in place, one rewrites it in Rust. Earlier
  RFCs compared two ports of a specification, which measures the case
  where WeaveC can at best draw.
- **(D6) Two annotations and about 1,700 lines.** `WEAVEC_ALIASED` and
  `WEAVEC_SAFE_FILE`. The library's line limit rises from 24,750 to
  26,500; the runtime's does not change.

## Summary

Six RFCs built Rust's loop in C and C++: rules an agent can iterate
against, a guarantee behind them, a fast build, no data races. Every
measurement of that loop was a port: a specification, written fresh by
an agent that chose its own data structures. That is the case where the
best possible result is parity with Rust, and it is not where most
rewrites start. The codebases that move to Rust are mostly C and C++
already. For them WeaveC can offer what Rust cannot: no rewrite. The
code stays where it is, its build, its tests and its callers stay, and
an agent converts it a file at a time until the compiler says it is
safe.

RFC 0044 made that offer in its second paragraph of motivation and
gated it (G5). The gate was never run. Run today, the offer fails
before an agent starts: four C++ projects of the corpus, 33,037 lines,
get 2,275 distinct errors when compiled as safe, and 93% of them come
from four rules that say a raw pointer may not be indexed, may not be
changed through, may not own and may not be cast. The rules about
borrows and moves, which are what Safe C++ is for, account for 2%. An
existing C++ program is not rejected for what it does with lifetimes.
It is rejected for containing pointers, and the only fix on offer is to
replace each with a library type, which changes signatures across the
project and is most of what a rewrite costs.

Safe C never had that problem. A safe C function indexes a pointer whose
declaration says how many elements it has, and the access is compared
with that bound. This RFC gives Safe C++ the same pointers and makes
converting a project a loop the tools carry:

- **Pointers with bounds.** `WEAVEC_COUNTED_BY` and the other bounds
  annotations work in C++ as in C, on parameters, results, members and
  globals. Arrays keep their size when they become pointers, a
  container's `data()` is bounded by the container, and a `std::span` or
  a `std::string_view` is made from a bounded pointer with one checked
  conversion. Each access is compared with the bound, or keeps its guard
  where the bound is not known in the function.
- **Aliased classes.** A class declared `WEAVEC_ALIASED` may be changed
  through a pointer another object keeps. The declaration closes a hole
  measured below: today the fix `owner-path` recommends,
  `std::shared_ptr`, lets safe code overwrite a `std::variant` under a
  live reference into it, silently, in both builds.
- **Safe files.** `WEAVEC_SAFE_FILE` at the top of a file makes it safe
  for every unit that includes it, so a header is converted once and the
  build system is not touched.
- **The survey and the plan.** `-fweavec-safe-survey` records, for every
  function that is not safe yet, the errors it would get, and
  `--weavec-audit` prints the files that are ready to mark and the rules
  that block the rest.

Nothing changes in the runtime. A unit that uses none of this compiles
to the same object file.

Expected result (gates, §10): the 26-line probe below compiles with one
annotation where it gets six errors today; the hole through
`std::shared_ptr` stops the compile; an agent converts yaml-cpp and
tinyxml2 in place with their own test suites passing, at least 80% and
70% of their functions safe and no change to a public signature but
annotations; and the conversion of tinyxml2 takes at most half the agent
time of its rewrite in Rust.

## Motivation

### Where rewrites start

RFC 0044 gave two reasons for C++. The second was "who rewrites": "The
codebases that move to Rust are often C++ already (browsers, databases,
operating system components) ... For the first group, safe C++ is not a
rewrite at all: it is a conversion in place, a file at a time, of code
whose tests, build and callers stay." Its gate G5 asked for two
conversions of existing C++ by agents. Its _Measured gates_ say: "Not
run. ... No agent converted a project." RFC 0045 and RFC 0046 did not
run it either. Every number this project has for the agent loop in C++
comes from a port of a specification (an LRU cache, a JSON module, an
indexer), where the agent wrote modern C++ from nothing.

A port is the case Rust is best at, and the results say so: equal
rounds, 1.2 to 1.95 times the wall time, 1.04 to 1.85 times the run
time. A conversion in place is the case Rust cannot enter. A team with
300,000 lines of C++ that chooses Rust pays for a second build system, a
boundary of bindings that moves for years, and tests rewritten or
bridged; the agent must re-architect every cyclic structure before the
first function compiles. If the same team can keep its code and have an
agent make it safe a file at a time, with the test suite green after
each file, the comparison is no longer between two languages. It is
between a rewrite and no rewrite.

### Measured: what stops existing C++

`build/rfc47/survey/run.sh` compiles each unit of four corpus projects
with `weavec-c++ -fsyntax-only -fweavec-safe -ferror-limit=0`, and
`uniq.py` counts each error once by its location (a header's errors
repeat in every unit that includes it).

| Project  | Units | Lines of `.cpp` | Distinct errors | Per 100 lines |
| -------- | ----: | --------------: | --------------: | ------------: |
| yaml-cpp |    30 |           5,829 |             167 |           2.9 |
| leveldb  |    38 |          10,413 |             918 |           8.8 |
| tinyxml2 |     1 |           3,019 |             270 |           8.9 |
| pugixml  |     1 |          13,776 |             920 |           6.7 |
| all      |    70 |          33,037 |           2,275 |           6.9 |

By rule:

| Rule | Errors | Share | What the code does |
| --- | ---: | ---: | --- |
| `index-single` | 1,204 | 53% | steps a pointer (635), indexes one (385), passes an array as a pointer (184) |
| `owner-path`, through a pointer kept in memory | 447 | 20% | calls a member function that is not `const` through a pointer member |
| `raw-owner` | 281 | 12% | `delete` (130), `new` (126), a raw pointer given to a smart pointer (15), others (10) |
| `pointer-cast` | 194 | 9% | converts to another pointer type (130), a base to a derived class (37), removes `const` (20), others (7) |
| `union-pointer`, `thread-share`, `variadic`, `not-modeled`, `lock-held`, `int-to-pointer`, `owner-path` (`const`) | 95 | 4% | |
| `stale-borrow`, `escape`, `call-alias`, `owner-moved` | 54 | 2% | |

The four rules at the top are 2,126 of 2,275 errors (93%). None of them
is a finding about the program. Each says that a construct is not part
of the dialect. The four rules at the bottom are the dialect's reason to
exist, and existing code meets them 54 times in 33,000 lines.

The same three rules buy little where they do apply.
`build/rfc47/survey/yield.py` runs RFC 0044's 110 seeded temporal bugs,
their fixed twins and its 90 correct files (`build/rfc44/g2`, `g3`) and
asks, for each rule, how many bugs it alone stops and how many correct
programs it alone rejects:

| Rule | Bugs it alone stops | Fixed twins it rejects | Correct files it rejects |
| --- | ---: | ---: | ---: |
| `stale-borrow` | 41 | 6 | 1 |
| `escape` | 23 | 4 | 2 |
| `owner-moved` | 8 | 1 | 0 |
| `call-alias` | 2 | 1 | 0 |
| `index-single` | 1 | 6 | 0 |
| `owner-path`, through a pointer kept in memory | 1 | 4 | 2 |
| `raw-owner` | 0 | 3 | 1 |

(81 of the 110 bugs stop by this script, which compares messages with
their quoted names removed; RFC 0046 counts 82.) The set is of temporal
bugs, so it is not where a rule about bounds would show its worth; what
it shows is that the three bans are the only reason nine of the 23
rejected twins are rejected, and that they catch two bugs.

### Measured: a header is converted by whoever includes it

Under `-fweavec-safe` "a header of your own is checked by each safe unit
that includes it" (the guide). leveldb's 38 units print 2,252 errors, of
which 918 are distinct: the others are 87 header errors seen again in
each unit. A unit cannot be made safe before every header it includes
is clean, and a header is safe in one unit and guarded in the next
according to a command line. An agent converting `db_impl.cc` first has
to convert, or mark class by class, most of the project's headers.

### Measured: the recommended fix has a hole

`owner-path`'s note says: "change an object from what owns it or is
passed it: take it as a parameter of this function, hold it by
'std::shared_ptr', make the member a reference, or keep this function
WEAVEC_GUARDED". `build/rfc47/probe/ptr-switch.cpp` is rejected with
that note. `shared-switch.cpp` takes its advice:

```cpp
struct Doc { std::variant<std::vector<long>, std::string> data; };
void clobber(const std::shared_ptr<Doc> &d) { d->data = std::string("AAAAAAAAAAAAAAAAAAAAAA"); }
long f(const std::shared_ptr<Doc> &a, const std::shared_ptr<Doc> &b) {
  const std::vector<long> &v = std::get<std::vector<long>>(a->data);
  clobber(b);                          // b is a
  return v.size() > 0 ? v[0] : -1;     // a vector's fields read from a string's bytes
}
```

It compiles as safe with no error. Called with one object for both
arguments it prints `-1` in the fast build and in the checked build,
with no report (`shared-noswitch.cpp`, the same program without the call
to `clobber`, prints `1`). RFC 0044 opened this path on purpose (its
decision D3: a change through a `std::shared_ptr` is "the one hole this
RFC opens on purpose"), counting on the guard at the stale borrow's use.
RFC 0045 then found the class of error where that guard does not help,
storage that changes type in place ("Today's guards are not a guarantee
here either: the bytes are live"), and its (Y6) lists the rules that
stand in front of it: "`stale-borrow` ... in the function, `call-alias`
and `owner-path` across calls". Through a `std::shared_ptr` none of the
three applies, and RFC 0045 did not say so. So the rule that produces a
fifth of the errors on existing C++ is stricter than the alternative it
recommends, and the alternative is unsound in the one place the run time
cannot help.

### What an agent meets today

`build/rfc47/probe/slice.cpp`, 26 lines in the style of the survey's
projects:

```cpp
class Slice {
public:
  Slice(const char *WEAVEC_COUNTED_BY(n) d, std::size_t n) : data_(d), size_(n) {}
  char operator[](std::size_t i) const { return data_[i]; }
  void remove_prefix(std::size_t n) { data_ += n; size_ -= n; }
private:
  const char *WEAVEC_COUNTED_BY(size_) data_;
  std::size_t size_;
};
int sum(const std::vector<int> &v) {
  int s = 0;
  const int *p = v.data();
  for (std::size_t i = 0; i < v.size(); i++) s += p[i];
  return s;
}
unsigned decode(const char *p, const char *limit) {
  unsigned r = 0;
  while (p < limit) r = r * 10 + unsigned(*p++ - '0');
  return r;
}
int local() { char buf[8] = {}; std::memset(buf, 1, sizeof buf); return buf[3]; }
```

Six errors: `bad-annotation` ("'weavec.counted_by' is one of Safe C's
annotations, which a safe C++ function does not use"), `index-single`
four times, and "an array becomes a pointer that has lost its size".
Every note says to use `std::span` or to keep the function
`WEAVEC_GUARDED`. As C, the same code with the same annotations is
accepted and checked. Under this RFC the file compiles once `decode`
says `const char *WEAVEC_ENDED_BY(limit) p`.

### Why this is affordable now

The machinery exists. RFC 0041 and RFC 0042 built bounds for raw
pointers: the annotations (`[[clang::annotate_type]]`, which C++ parses
already), the table of each function's bounds by symbol (`SafeUnit`,
which is not specific to a language), the entry that validates a bounded
parameter, the comparison that replaces a guard, the field annotations
that bound a loaded pointer, the conversion checked at a store. RFC 0044
built kinds that are read from a class's structure, and RFC 0045 the
structural test for storage that switches type. C++ already treats a
`WEAVEC_BORROWED` pointer member as making its class a view. What is
missing is the decision that C++ may use them, and the C++ forms
(members, constructors, `this`, templates) the C rules never met.

## Soundness

### The guarantee, restated

For a safe C++ function, in a program that meets the assumptions below:

- **(X5), replaced. No pointer outside its bound.** A pointer is
  indexed or stepped only when a declaration, an array, an allocation or
  the library table gives it a bound, and each access through it is
  inside that bound: a comparison with the bound where the function
  knows it, the access's guard where it does not. No pointer is made by
  a cast from an integer or from a pointer to an unrelated type; a
  conversion to bytes (§3) reads or writes only the object's own bytes.
- **Typed memory, in C++.** A pointer member with a bound is, wherever
  safe code can load it, null or within that bound: every store a safe
  function makes to it or to its count keeps it so (`counted-pair`,
  `call-args`), and a safe function that loads it believes it, as RFC
  0042's (M1) says for C.
- **(X3), narrowed (D2).** A safe function does not use a borrow or a
  view after an owner it points into was destroyed, moved, assigned or
  changed by the function itself or by a call that was handed that
  owner. A call that reaches the owner through a pointer kept in memory
  can change it only if the owner is, or belongs to, an object of an
  aliased class; such a change is not followed, and is (X6).
- **(Y6), corrected (D2).** Storage that changes type in place is not
  changed through a pointer kept in memory or a `std::shared_ptr`: an
  aliased class owns none but a string's characters, and a borrow
  through which a string's characters can be written does not outlive a
  call unless the string is the function's own local. With that, the
  sentence RFC 0045 wrote is true: a borrow into switching storage is
  ended by a rule in the function that switches it or in a caller that
  handed it over, and by nothing else.
- **(X6), amended.** Its first entry reads "through a reference, or into
  what an aliased object owns after a call changed it through one". Its
  entry for `std::shared_ptr` is narrowed to objects of aliased classes.
  In the checked build each still stops when it runs, by (T) and (A); in
  the fast build a stale pointer reaches memory of its type (Y2).

(X1), (X2) and (X4) are unchanged, but (X1) now allows a `new` whose
block a smart pointer takes in the same expression.

### Bugs caught

At compile time, each by a rule that exists:

```cpp
struct Buf { char *WEAVEC_COUNTED_BY(cap) p; std::size_t cap; };
void shrink(Buf &b, std::size_t n) { b.cap = n; }            // counted-pair
int sum(const int *WEAVEC_COUNTED_BY(n) v, std::size_t n);
int one(const int *p) { return sum(p, 4); }                  // call-args
int third(const int *p) { return p[2]; }                     // index-single
struct Doc { std::variant<std::vector<long>, std::string> data; };
void clobber(const std::shared_ptr<Doc> &d) { d->data = std::string("x"); }  // owner-path
void fill(std::string &name, Log &log) {
  char *p = name.data();
  log.flush();
  *p = 'x';                                                  // stale-borrow
}
```

When it runs, in both builds: an index or a step past a declared bound
(`pointer-out-of-bounds`), a pointer stored in a counted member that
does not cover its count (`bounds-conversion`), a bounded argument from
code that is not safe that does not cover its count (the entry's range
guard).

### Bugs deliberately not caught

- **A dangling pointer member with a bound.** It is a reference (RFC
  0043): tested when a safe function loads it, and in the fast build a
  released block of its type passes the test (Y2). Declared
  `WEAVEC_BORROWED` it is a view's, and the rules follow it instead.
- **A borrow into what an aliased object owns, used after a call changed
  the object through a pointer.** The Rust program would be an
  `Rc<RefCell<T>>` that panics. Here the checked build reports a
  `heap-use-after-free` when a test reaches it, and the fast build reads
  memory of the borrow's type.
- **A count that code which is not safe set wrong**, or a counted member
  it left dangling: trusted, as in C, listed by the audit, and checked
  under `-fweavec-safe-checks=full` and `verify`.
- **Raw owners**: still not written in safe C++ (D3), so still not a
  class of bug there.

### Accepted rejections

- **A shared object that holds a variant and changes.** A class that
  owns a `std::variant` with an alternative that holds a pointer, a
  `std::any` or a `std::function` cannot be aliased, so it is not
  changed through a `std::shared_ptr` or a pointer member. Replace the
  whole object (`p = std::make_shared<Doc>(...)`), pass it as a
  parameter, or keep the function guarded. Rust accepts this program
  with a run-time count; a checked cell is _Future work_.
- **Programs that changed any object through a `std::shared_ptr`**
  compile today and need one annotation on the class (a breaking
  change), or are rejected by the entry above.
- **A writable pointer into a string that is not a local, kept across a
  call**: `char *p = dst->data(); helper(); *p = 'x';`. Take the pointer
  again after the call, or keep an index.
- **A bounded pointer captured by a lambda** is one object inside the
  lambda; capture a `std::span`.
- **A reference or a pointer to a pointer that has a bound** is not
  made (`field-address`): the count would not follow it.
- **A false trap, as in C:** code that is not safe passes a count larger
  than the object it points to, to a safe function that would not have
  read that far. The entry stops it (`pointer-out-of-bounds`), since the
  function believes the bound from then on.

### Assumptions

X-A1 to X-A5 and Y-A1 to Y-A5 stand. Added:

|          | Assumption |
| -------- | ---------- |
| **X-A6** | Code that is not safe keeps each pointer member that has a bound null or within it, and gives a safe function it calls bounded arguments its entry can validate (RFC 0042's assumption for C, now for C++). |
| **X-A7** | Code that is not safe does not change storage that switches type through a pointer kept in memory or a `std::shared_ptr` while safe code holds a borrow into it. |

## Detailed design

### 1. Pointers with bounds in safe C++ (D1)

#### 1.1 Where a bound comes from

As RFC 0041 §2 and RFC 0042 §5, with the C++ forms added:

- **A declaration.** `WEAVEC_COUNTED_BY(n)`, `WEAVEC_SIZED_BY(n)`,
  `WEAVEC_ENDED_BY(e)`, `WEAVEC_NULL_TERMINATED` and their `_OR_NULL`
  forms, after the `*`, on a parameter, a result, a data member or a
  global. On a parameter `n` and `e` name another parameter of the
  function or a constant; on a member, a sibling member of the same
  class (not of a base); on the result of a member function that is not
  static, a parameter, a constant or a data member of the class
  (`const char *WEAVEC_COUNTED_BY(size_) data() const`). A virtual
  function and each function that overrides it give the same bounds, as
  every declaration of one function does.
- **An array.** A local, member or global array used as a pointer has
  its length. "An array becomes a pointer that has lost its size" is no
  longer an error.
- **The library table.** A class row may say `counts(data, size)`: the
  result of member `data` has as many elements as member `size` returns
  at that moment. Rows are added for the contiguous containers,
  `std::basic_string` (`data`, and `c_str` as a string),
  `std::basic_string_view`, `std::span` and `std::array`. Such a pointer
  is a borrow of the container's elements, as it is today, so
  `stale-borrow` ends it when the container changes.
- **A local.** A local pointer has bounds when everything assigned to it
  does, as in C. `&x`, `&v[i]` and `this` point to one object.
- **`main`.** `argv` has `argc` elements, each a string. RFC 0044's
  exemption for `main` is removed.

A pointer with none of these points to one object, in both languages.

#### 1.2 The rules

No rule is added. C's rules lose "C only":

| Rule | In a safe C++ function |
| --- | --- |
| `index-single` | A pointer without a bound is not indexed or stepped. The note names the bound to declare first and `std::span` second. |
| `call-args` | An argument for a parameter with a bound has bounds that cover it; a pointer to one object is not passed, stored or returned where a declaration gives a bound. |
| `counted-pair` | A member that bounds a pointer member changes only next to its pointer's assignment, or alone by a step that is checked. A constructor's member initializers for the pointer and its count are one pair, wherever they stand in the list; so are an aggregate's initializers and the default member initializers. A defaulted copy or move keeps the pair. |
| `string-index`, `string-write` | As C, for a `WEAVEC_NULL_TERMINATED` pointer. |
| `field-address` | The address of, or a reference to, a pointer member that has a bound, or its count, does not leave the function and is not bound to a reference parameter. |
| `bad-annotation` | No longer rejects the bounds annotations in C++. Still rejects `WEAVEC_OWNED` and `WEAVEC_DROPS` there (D3), a bound that names nothing, and declarations that disagree. |

A lambda's capture of a bounded pointer is a pointer to one object. A
template is checked per instantiation, as today.

The code that reads a declaration's bounds and decides the bounds of an
expression moves out of `SafeCheck.cpp` into one file that both checkers
use; `SafeCxx.cpp`'s own `index-single` is deleted.

#### 1.3 Pointers and spans meet

RFC 0044 says "a span is made from a container or an array, not from a
pointer and a size". That sentence goes. The table marks the
pointer-and-count parameters of the library's constructors and members
(`std::span(p, n)`, `std::string_view(p, n)`, `std::string(p, n)`,
`append(p, n)`, `assign(p, n)`, the iterator-pair forms given two
pointers) with a `counted(i, j)` clause, and `call-args` then asks what
it asks of any bounded parameter: the argument has bounds that cover
`n`, compared when it runs. A `WEAVEC_NULL_TERMINATED` pointer makes a
`std::string_view` or a `std::string`. In the other direction `data()`
is bounded (§1.1). A function converted with annotations and a function
written with `std::span` call each other with no guarded code between
them.

#### 1.4 At run time

A safe C++ function with a bounded parameter, or that loads a bounded
member, gets what a safe C function gets from `GuardSafe.cpp` and
`GuardTyped.cpp`, except heap placement:

- its entry in `SafeUnit::functions`, by mangled name, with its
  parameters' and result's `PointerBounds`; `PointerBounds` gains the
  form "counted by the integer at an offset from the first argument" for
  a result bounded by a member;
- the split of RFC 0041's amendment 18: a public entry that validates
  bounded parameters against the shadow, and a body that safe callers
  with rooted arguments call after one comparison;
- for each access whose pointer derives from a root with known bounds (a
  validated parameter, an array, a member's annotation, a call the table
  or the callee's declaration bounds by an argument or a constant), a
  comparison in place of the guard, `pointer-out-of-bounds` when it
  fails;
- the field annotations of RFC 0042 §5 on C++ classes and on the
  instantiations of class templates, so that a pointer loaded from a
  bounded member is compared with its count;
- the conversion of RFC 0042 at a store to a bounded member, at a call
  and at a return (`bounds-conversion`);
- RFC 0043's test of a reference when it is loaded, for a bounded
  pointer member that is not `WEAVEC_BORROWED`.

Every other access keeps its guard, as (Y3) says of "a pointer computed
by arithmetic" today: the result of `data()`, whose bound is a call the
function cannot repeat, is one. So no access this section allows is
unchecked; what the bound buys where the function knows it is an exact
check that costs one comparison.

RFC 0041's heap placement of locals whose address escapes does not
apply to C++: `escape` and (Y4) already cover a C++ local, and a C++
object is not moved to the heap behind the program's back.

`-fweavec-safe-checks=full` keeps every guard beside the comparisons,
and `verify` keeps each replaced guard as a monitor, as for C.

### 2. A pointer member is a view's or a reference (unchanged, now useful)

RFC 0044 lets a class choose: a reference member or a
`std::string_view` makes it a view, which the rules follow and tie to a
scope; a raw pointer member is a reference, which may live anywhere and
is checked when it runs. `WEAVEC_BORROWED` on a pointer member already
puts it in the first group. With bounds the choice covers the classes
existing code has:

```cpp
class Slice {                    // a view: followed by `stale-borrow` and `escape`
  const char *WEAVEC_BORROWED WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};
class Cursor {                   // a reference: stored anywhere, tested when loaded
  const char *WEAVEC_COUNTED_BY(size_) data_ = nullptr;
  std::size_t size_ = 0;
};
```

The guide states the choice in these words. Nothing is built for it but
the pages.

### 3. Casts

C's `pointer-cast` is more permissive than C++'s and is the model:

- A pointer to an object converts to `const char *`,
  `const unsigned char *` or `const std::byte *`, by `reinterpret_cast`
  or a C-style cast, and the result is sized by the object (or by its
  bound times its element size).
- If the object's type is trivially copyable and holds no pointer, the
  same conversion may drop the `const`.
- Everything else `pointer-cast` rejects in C++ today stays rejected: a
  base converted to a derived class without `dynamic_cast`, a
  `const_cast`, a conversion from bytes to an object type (`memcpy` into
  an object, which `pointer-bytes` allows for trivially copyable types,
  is the checked form).

### 4. Owners (D3)

`raw-owner` keeps its rule and loses one case: a `new` expression that
is the argument of a `std::unique_ptr` or `std::shared_ptr` constructor
or of `reset` is accepted (`std::unique_ptr<T>(new T(...))`,
`p.reset(new T)`), since no path exists on which the block has no owner.

Raw owners are otherwise not brought to C++. The survey's 256 `new` and
`delete` sites are of two kinds. A member the class allocates and its
destructor deletes becomes a `std::unique_ptr`, an edit inside the
class, the one a Rust port makes (`Box`), with `get()` where a raw
pointer went out. A class that counts its own references and deletes
itself, a pool, an arena, placement `new`: these are implementations of
owners, and RFC 0044 already says they are `WEAVEC_GUARDED`. Following
`WEAVEC_OWNED` pointers in C++ would need RFC 0043's states (null, moved
by copy, a leak on every path) in a flow that has exceptions, for the
first kind only. A function that must keep returning a raw owner to
callers outside the project is a guarded line (`return p.release();`).
Gate G3 records what share of each conversion's guarded functions are
guarded for a raw owner; that number decides whether this is revisited.

### 5. Aliased classes (D2)

#### 5.1 The declaration

`WEAVEC_ALIASED` goes where `WEAVEC_SAFE` goes on a class:
`class WEAVEC_ALIASED Env { ... };`. A class derived from an aliased
class is aliased. It says: objects of this class are changed through
pointers that other objects keep.

#### 5.2 Three rules, under two existing names

**(W) Who may change through a pointer.** `owner-path`'s C++ clause
becomes: through a raw pointer kept in a member or an element, or
through a `std::shared_ptr`, a safe function calls a member function
that is not `const`, or assigns a member that owns something, only on an
object whose class is aliased, or whose type is a library type none of
whose storage switches (a `std::vector<int>`; not a `std::string`, not a
`std::vector<std::variant<...>>`). The `std::shared_ptr` half is new and
is the breaking change.

**(K) What an aliased class may own.** Nothing an aliased class owns
(its members and bases, the elements of its containers, what its
`std::unique_ptr` and `std::optional` members hold, to any depth) is
storage that switches type in place, except a string's characters. The
test is RFC 0045 amendment 13's (`CxxKinds::switchesFor`), asked for
every pointee type but a character type. An owned object of a
polymorphic class must itself be of an aliased class, since a class
derived from it could add such storage. (K) is checked where the class
is complete (and where a class template is instantiated), and is
`owner-path`.

**(S) Strings.** A string is in every class, and its characters share
bytes with its pointer. A borrow through which the characters of a
`std::basic_string` can be written (a `char *`, a `char &`, a
`std::span<char>`) into a string the function does not reach through its
own local or by-value parameter ends at any call the function makes, and
at any change it makes through a pointer kept in memory or a
`std::shared_ptr`. Using it afterwards is `stale-borrow`. A `const`
borrow of characters is not affected: reading stale characters is not a
memory error (RFC 0045 §7).

#### 5.3 Why that is enough

A borrow into switching storage can be made wrong in place only by
changing the object that holds the storage. A change from the function
itself, or by a callee handed the object, is followed by `stale-borrow`
and `call-alias` today. A change through a pointer kept in memory or a
`std::shared_ptr` touches, by (W), only an object of an aliased class
and what it owns, which by (K) holds no switching storage but strings'
characters, which (S) covers. A pointer member of an aliased object
leads to another object, and (W) applies again. What remains is what
(X6) lists already: a stored callable, a global, code that is not safe.

What (W) gives up is narrower than it looks. A call through a pointer
can now clear a vector that a caller holds a reference into. That
program compiled before too, if the object was shared by
`std::shared_ptr`, and it is the kind of stale pointer both builds were
designed for: reported in the checked build, of its own type in the fast
one. The kind neither build can catch is the one (K) and (S) remove.

#### 5.4 What the errors say

`owner-path` at a change through a pointer names the class and the
declaration first:

```text
env.cc:41:10: error: 'this->env_' is reached through a pointer kept in memory, and 'NewWritableFile' may change what it owns [weavec-safe:owner-path]
env.cc:41:10: note: declare 'Env' WEAVEC_ALIASED if its objects are changed through pointers other objects keep; or own it, take it as a parameter, or make the member a reference
```

The note no longer recommends `std::shared_ptr`.

### 6. Safe files (D4)

`WEAVEC_SAFE_FILE`, written once after `#include <weavec.h>` at the top
of a source or header file, makes safe every function whose definition
starts in that file after it, the member functions of every class
defined there, their lambdas and the instantiations of the templates
defined there. It expands to `_Pragma("weavec safe")` under WeaveC and
to nothing under other compilers. It does not reach files the file
includes. It works in C and in C++.

`-fweavec-safe` keeps its meaning (every file of the unit that is not a
system header is safe), which is what a new program wants.
`WEAVEC_GUARDED` and `WEAVEC_SAFE` on a function or a class keep theirs.
The driver registers the pragma's handler where it wraps cc1; an unknown
`#pragma weavec` is `bad-annotation`.

A unit with a safe file and no flag is a safe unit for RFC 0045's
purposes only if every function it defines in its own files is safe;
otherwise its safe functions are compiled as `WEAVEC_SAFE` functions in
a guarded unit are today. The fast mode therefore arrives when a unit's
files are all converted, which the audit shows.

### 7. The survey and the plan (D4)

`-fweavec-safe-survey` (with `-fweavec-ledger=<dir>/`) runs the rules
over every function of the unit's own files that is not safe, as if it
were. Their errors are not printed and do not fail the compile. The
ledger records, per function, its file and the count of errors by rule.
Safe functions are checked as always.

`weavec-cc --weavec-audit <dir>` then prints, before its lists:

```text
conversion: 412 of 1,930 functions safe (21%); 388 more have no error
ready (mark WEAVEC_SAFE_FILE):
  util/arena.h              6 functions
  util/coding.cc           31 functions
blocked, fewest errors first:
  table/block.cc            4 errors: index-single 3, pointer-cast 1
  db/memtable.cc            9 errors: owner-path 6 (MemTable 4, Arena 2), raw-owner 3
by rule: index-single 211, owner-path 96 (by class: Env 31, VersionSet 22, ...), raw-owner 80
```

A file is ready when no function it defines has an error; `owner-path`
errors are grouped by the class an annotation would name. The numbers
are the progress of a conversion: what a team reports the way it would
report the share of a codebase that is in Rust. The survey costs what
the rules cost (RFC 0044 G7: at most 3% of `-fsyntax-only`).

### 8. The ledger, the audit, explanations, the guides

- The ledger's version is 10: a function's `survey` counts; a class's
  `aliased`; a member's kind `counted` beside `reference`, `borrowed`
  and `owner`. The audit's version is 6: the conversion table, and the
  aliased classes listed with the classes that are aliased by
  derivation, since they are what (X3) no longer covers across calls.
- `resources/safe/rules.md`: _In C++_ parts for `call-args`,
  `counted-pair`, `string-index`, `string-write` and `field-address`;
  rewritten ones for `index-single`, `pointer-cast`, `bad-annotation`,
  `owner-path`, `raw-owner` and `stale-borrow`; `safe-cxx` says what a
  pointer with a bound and an aliased class are. Each example compiles
  under `docs/scripts/check-examples.py`.
- A new guide, `docs/pages/guides/convert-cxx.md`: the loop (survey,
  mark the ready files, take the blocked file with the fewest errors,
  run the project's tests in the checked build, repeat), a bound or a
  span, a view or a reference, `std::unique_ptr` for an owned member,
  when a class is aliased. `adoption.md` points to it.
  `safe-cxx.md`'s Rust table changes two rows: `&[T]` is a `std::span`
  or a pointer with a bound, and `Rc<RefCell<T>>` is a
  `std::shared_ptr` to an aliased class.
- `docs/pages/reference/guarantees.md`: (X3), (X5), (X6) and (Y6) as
  restated above, X-A6 and X-A7; `docs/annotations.md` and the
  command-line reference for the two annotations and the flag.

### 9. Cleanups and breaking changes

- **Breaking:** changing an object through a `std::shared_ptr` needs its
  class to be `WEAVEC_ALIASED` (W); an aliased class may not own a
  variant, an `any` or a `function` (K); a writable pointer into a
  string that is not a local does not cross a call (S).
- **Breaking:** the ledger's and the audit's versions; `weavec.h` 3.3.
- **Removed:** `SafeCxx.cpp`'s copy of `index-single` and its `main`
  exemption; `bad-annotation`'s C++ ban on bounds; the rule pages' and
  notes' advice to convert every pointer to a `std::span` and to hold a
  changed object by `std::shared_ptr`; RFC 0044's gate G5, which this
  RFC's G3 replaces.
- **Moved:** the reading of bounds from declarations, shared by both
  checkers.
- **Not changed:** Safe C; a C++ unit that uses no bound, no aliased
  class and no safe file (its object file is the same); the runtime and
  its ABI; the fast and the checked build.

### 10. Stages and gates

- **S0 Before code.** (a) The rules of §1 and §5 written out by hand
  against leveldb's `Slice`, `coding.cc` and `Arena`, tinyxml2's
  `StrPair` and `XMLUtil`, and yaml-cpp's `Stream`. (b) The 447
  `owner-path` errors classed by the class behind the pointer: owned by
  the class that holds the pointer, eligible to be aliased, or stopped
  by (K). (c) RFC 0044's and RFC 0046's accepted sets and the three
  ports searched for what (W) and (S) would newly reject. (d) The 130
  pointer conversions classed by §3. (e) A frozen set of forty spatial
  bugs on raw pointers in C++ with fixed twins, written by an agent that
  has not seen this RFC and confirmed under AddressSanitizer. S0 may
  reopen (D2) and (D3) and sets the thresholds below.
- **S1** Bounds in the checker. **S2** Bounds at run time. **S3** The
  table's `counts` and `counted` clauses, views by `WEAVEC_BORROWED`.
  **S4** Casts and `new` into a smart pointer. **S5** Aliased classes.
  **S6** Safe files, the survey, the ledger and the audit. **S7** The
  pages and the guides. **S8** The gates.

Gates (thresholds are proposals until S0):

- **G1 Tests.** Each rule's C++ part is pinned in `test/Safe/`
  (`cxx-bounds.cpp`, `cxx-counted-pair.cpp`, `cxx-aliased.cpp`,
  `cxx-string-borrow.cpp`, `cxx-casts.cpp`, `safe-file.cpp`); the
  comparisons, the entries and the member bounds in `test/Guards/`
  (`ir-cxx-bounds.cpp`, `cxx-bounds.cpp` with the exact
  `pointer-out-of-bounds` line); the survey, the ledger and the audit in
  `test/Driver/safe-survey.cpp`; the table's rows in `LibrarySpecTest`.
- **G2 The probes.** `slice.cpp` compiles with one annotation added and
  each of its accesses is compared or guarded; `shared-switch.cpp` stops
  the compile; every one of S0's forty spatial bugs stops the compile or
  stops with a report in both builds, and at least 36 fixed twins
  compile.
- **G3 Conversions in place.** yaml-cpp and tinyxml2 converted by agents
  given the guide, `--weavec-explain` and the audit; leveldb recorded,
  not gated. The project's own test suite passes in the checked build
  with no report and in the fast build. No public declaration changes
  but by an annotation, so the project's tests and examples compile
  against the converted headers unedited. At least 80% of yaml-cpp's
  functions and 70% of tinyxml2's are safe. Recorded: rounds, agent
  time, lines changed, annotations by kind, guarded functions by reason
  (a raw owner, a cast, a change through a pointer, other).
- **G4 Against a rewrite (D5).** tinyxml2 (3,019 lines and its header)
  rewritten in Rust by an agent with the same brief and the same test
  suite to pass, which it must port. The conversion takes at most half
  the rewrite's agent time. Recorded: both programs' lines, tests
  passed, and tinyxml2's own benchmark where it has one.
- **G5 Cost.** Each converted project's tests and one benchmark per
  project, fast build, at most 1.3 times the same source compiled by
  Clang; no converted unit slower than it was guarded.
- **G6 Defects found.** Every error an agent classes as a defect in the
  original is reproduced under the guards and recorded.
- **G7 What was accepted before.** RFC 0044's 110 bugs: at least 81
  still stop. Its 90 correct files, RFC 0046's 40 and the three ports:
  at most three newly rejected, each fixed by one annotation or one
  line; the count of fixed twins rejected (23) does not rise.
- **G8 The survey again.** The four projects, unedited: none of the 184
  errors for an array used as a pointer remains, no rule's count rises
  but `owner-path`'s by (W), and the new counts are recorded beside the
  table in _Motivation_.
- **G9 Nothing elsewhere.** Safe C's tests, cases and conversions as on
  main; a C++ unit without the new constructs compiles to the same
  object; the corpus gate unchanged.
- **H1 Hygiene.** The library within 26,500 lines; the runtime
  unchanged.

## Annotation surface

`resources/include/weavec.h` becomes version 3.3.

- **`WEAVEC_ALIASED`** (new): after `class` or `struct` in a class
  definition, C++ only. Expands to `annotate("weavec.aliased")` and to
  nothing under other compilers. In C it is `bad-annotation`.
  `AliasedAnnotation` in `include/weavec/Frontend/SafeCheck.h`.
- **`WEAVEC_SAFE_FILE`** (new): at file scope. Expands to
  `_Pragma("weavec safe")` and to nothing under other compilers.
- **`WEAVEC_COUNTED_BY`, `WEAVEC_SIZED_BY`, `WEAVEC_ENDED_BY`,
  `WEAVEC_NULL_TERMINATED`** and their `_OR_NULL` forms: unchanged
  spellings, now read in C++, on parameters, results, data members and
  globals. A member function's result may name a data member.
- **`WEAVEC_BORROWED`**: unchanged; with a bound it makes a view's
  member.
- **`WEAVEC_OWNED`, `WEAVEC_DROPS`**: C's, as before.

`docs/annotations.md` mirrors each.

## Diagnostics

No rule identifier and no report kind is added. Messages added or
changed, each pinned in `test/Safe/`:

```text
error: 'p' is indexed, and it points to one object [weavec-safe:index-single]
note: give 'p' a bound (WEAVEC_COUNTED_BY, WEAVEC_ENDED_BY), or take a 'std::span'
error: 'size_' bounds 'data_' and is assigned without it [weavec-safe:counted-pair]
error: a reference to 'data_', which has a bound, would leave its count behind [weavec-safe:field-address]
error: 'doc' is reached through a 'std::shared_ptr', and 'operator=' may change what it owns [weavec-safe:owner-path]
note: declare 'Doc' WEAVEC_ALIASED if its objects are changed while shared; or replace the whole object
error: 'Document' is WEAVEC_ALIASED and owns 'root_', whose contents change type in place [weavec-safe:owner-path]
note: an aliased class holds no 'std::variant', 'std::any' or 'std::function'; change 'Document' from what owns it instead
error: 'p' points at the characters of 'name', which the call to 'flush' may have moved [weavec-safe:stale-borrow]
note: take 'p' again after the call, or keep an index
error: 'WEAVEC_ALIASED' is on a C struct [weavec-safe:bad-annotation]
```

At run time a safe C++ function can now end in `pointer-out-of-bounds`
and `bounds-conversion`, the kinds RFC 0041 and RFC 0042 defined, with
their existing lines; `test/Guards/cxx-bounds.cpp` pins them for C++.

## Drawbacks

- **A raw pointer is back in the dialect.** RFC 0044 could say "a raw
  pointer in safe C++ is only ever a borrow of one object". It can no
  longer. The bound is declared and checked, as in C, but a reader must
  now know two ways to say "some elements".
- **(X3) is weaker for programs that opt in.** A team that aliases its
  manager classes has accepted, for what those objects own, a stale
  borrow that only a test in the checked build finds. Rust does not make
  that trade. The audit lists the classes; nothing lists the borrows.
- **Shared mutable variants are rejected outright**, which is stricter
  than Rust and than today.
- **(S) will reject correct code** that fills a caller's string through
  a pointer across helper calls. S0 counts how much.
- **Annotations no model was trained on.** A model has read a great deal
  of `std::span` and little of `WEAVEC_COUNTED_BY`. The bet is that an
  annotation on an existing line, named by the error's note, is easier
  than a signature that changes in forty files.
- **C++ forms the C passes never met**: member initializers, `this`,
  references, templates, invokes in `GuardSafe.cpp`. Each is a place for
  a wrong comparison, which `verify` monitors but tests must reach.
- **About 1,700 more library lines** and a limit raised a seventh time
  (D6).
- **The gate is expensive**: three conversions and a rewrite by agents.

## Alternatives

- **Keep the bans and let agents convert to `std::span`.** No new
  construct. It is what the notes say today, and it is the rewrite:
  1,204 sites whose signatures ripple through headers and public
  interfaces. Nothing suggests an agent cannot do it; everything
  suggests it costs what moving to Rust costs, and then Rust's ecosystem
  decides.
- **Allow pointer arithmetic with no bound, under the guard.** The
  access is checked either way. But the guard knows where objects end,
  not where the program meant to stop, costs a shadow read where a
  comparison would do, and gives the agent no error to act on. A
  declared bound is what Rust's slice is.
- **Allow change through any pointer, with no declaration.** It removes
  447 errors and, with them, the one rule that stands in front of
  switching storage across calls. The probe shows what that costs.
- **Close the `std::shared_ptr` hole alone**, by rejecting every change
  through one. Sound and simple, and it leaves safe C++ with no shared
  mutable state at all.
- **A checked cell** (a borrow count, as `RefCell`). The right long-term
  answer for shared mutable variants; a library type WeaveC would ship,
  a run-time cost, and no help to code that already exists.
- **`WEAVEC_OWNED` in C++.** Argued in §4. Deferred until G3 says how
  many functions it would free.
- **A list of safe files in the build** instead of a marker in the file.
  No edit to sources, but the build does not rebuild when the list
  changes, and the claim is no longer visible where the code is read.
- **Another port.** A seventh measurement of the case that is already
  near parity.

## Prior art

- **Clang's `-fbounds-safety`** (and the kernel's `__counted_by`): the
  model RFC 0041 took for C. It is C only upstream; this RFC is its
  extension to C++ members and member functions, with run-time checks
  the passes already have.
- **C++ safe buffers** (`-Wunsafe-buffer-usage`, hardened libc++): the
  position RFC 0044 took, "a pointer and a count is a `std::span`". It
  is the right target for new code, and Chromium's and WebKit's
  migrations show what it costs existing code: years of fix-its. We keep
  it as the second half of every note.
- **Checked C**: `_Array_ptr<T>` with declared bounds and incremental
  conversion file by file (`#pragma CHECKED_SCOPE`), and 3C, its tool
  that ranks what to convert next. The file marker and the plan are
  theirs. Checked C had no temporal story; WeaveC's is the earlier RFCs.
- **Rust**: `Cell` and `RefCell` are types that say "changed while
  shared", and `Rc<RefCell<Enum>>` is why Rust needs the count. An
  aliased class is the declaration without the count, made sound by
  keeping enums out of it.
- **Circle's Safe C++ (P3390)** and **C++ profiles**: a new language on
  one side, checks without a guarantee on the other. Neither converts
  existing code in place with a statement of what holds.
- **Fil-C**: all of C++ safe at run time with no edits, at a cost in
  time and memory this project measured itself against in RFC 0045. It
  is the far end of the line this RFC moves along: the more of existing
  C++ the rules accept, the less a conversion is a rewrite.
- **C2Rust and the translation agents** behind the rewrites this RFC
  answers: what they spend on re-architecture is the budget G4 compares
  against.

## Unresolved questions

1. **(D2) or nothing.** If S0 finds that most of the 447 sites are
   owned members that become `std::unique_ptr`, aliased classes may not
   be worth their rules, and the `std::shared_ptr` hole would be closed
   another way. If it finds that (K) stops many of them, the design is
   wrong.
2. **(S)'s false errors**, and whether a callee that is handed only the
   character pointer can be exempted without a hole.
3. **(K) by structure under libstdc++**, whose `std::string` and
   `std::variant` are laid out differently, and for `std::function` and
   `std::any`, which RFC 0045 does not see by structure.
4. **A result bounded by a member** needs the caller to load the count
   after the call. Is that sound when the callee changes the count? The
   fallback is the guard.
5. **Entries for virtual and indirect calls**: every such call reaches
   the validating entry, at the cost of a range check. How often is
   that on a hot path in converted code?
6. **Does a unit with one unconverted function lose the whole fast
   mode** (§6), or can believed pointers be decided per function?
7. **Bounds on locals.** Are derived bounds enough in C++, where a
   pointer local is often a reference or `auto`?
8. **`static_cast` to a derived class** (37 sites): a checked form
   without RTTI?
9. **What an agent does with a blocked header**: mark classes, or split
   the file?
10. **Which project stands for "large"** once leveldb has been read by
    the survey. The gate should be run on one this RFC never saw.

## Future work

- **A checked cell** for shared mutable variants, and with it a run-time
  count for aliased classes in the checked build.
- **`WEAVEC_OWNED` in C++**, if G3's guarded functions say so; then one
  ownership flow for both languages, and `SafeOwnership.cpp` retired.
- **Fix-its** for the notes that name an annotation, and a mode that
  applies them: `index-single`'s and `owner-path`'s are one token each.
- **Converting C to safe C++**: a C file that is safe C is, with this
  RFC, nearly safe C++; what remains is RFC 0043's owners.
- **The fast mode per function**, so that a half-converted unit is
  half fast.
- **RFC 0046's list**, unchanged: a run-time backstop for the thread
  rules, disjoint loans, the fixed twins' causes.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what the gates found. They override the body.

1. **Where the bounds live (§1.2).** The reading of declarations' bounds
   and the rules of bounds stay in `SafeCheck.cpp` rather than moving to a
   file of their own: Safe C++ calls Safe C's rule visitor in a C++ mode
   through three callbacks (`BoundsRules` in `SafeCheckImpl.h`: a
   declaration's table, the rules over one function, a class's bounds).
   In that mode the visitor checks `index-single`, `call-args`,
   `counted-pair`, `string-index`, `string-write` and `field-address`
   only, over the function's body, its constructor's list and its
   lambdas; Safe C++ keeps its own casts, unions, variadics and `escape`.
   `SafeCxx.cpp`'s `index-single`, its `main` exemption and its rejection
   of a span made from a pointer and a size are deleted.
2. **Which C++ functions the passes check by bounds (§1.4).** Only a safe
   function that uses a bound: it indexes or steps a pointer (not an
   iterator the library spells as a pointer), passes an array that is not
   of `const` characters as a pointer, names a member with a bound, or
   declares one. Every other safe C++ function is compiled exactly as
   before, which is what lets G9 hold. `main` with `argv` is always such a
   function: its `argv` is counted by `argc`, in C++ only.
3. **Pointers without a bound in C++ are not checked at the entry or the
   return.** RFC 0042 checks a single pointer parameter's object at the
   entry and a single result at the return; in C++ an end is routinely
   returned as one (`const char *end() const { return data_ + size_; }`),
   which that check stops. In C++ a pointer without a bound keeps its
   guards instead, as (Y3) says.
4. **The end that `WEAVEC_ENDED_BY` names is not an object**, in C as in
   C++: neither the entry nor a safe caller checks it as one. Before, a
   call `decode(text, text + 6)` on a six-byte array stopped with a false
   `bounds-conversion` in Safe C (since RFC 0042). A bug fix, pinned by
   `test/Guards/safe-ended-end.c`.
5. **The library's pointer-and-count parameters (§1.3)** are checked at
   compile time (`call-args`). When the program runs, the library's own
   accesses are guarded (its inline code) or checked by the table's rows
   (its out-of-line members), not by a comparison at the call. The class
   rows are `counts(data, size)`, `counted(member, i, j)` and `ended(member,
   i, j)` (the class's own name for a constructor), and `strings(c_str)`;
   a clause applies when argument `i` is a pointer and `j` an integer
   (`counted`) or a pointer (`ended`). An error names a library parameter by
   its place (`argument 1 of 'span'`).
6. **Field annotations in C++ (§1.4)** are given only to the classes with
   a member that has a bound, and only to that member and its count, so
   that a class without one compiles as before. A member function's result
   counted by a member reads the count from its first argument after the
   call (`PointerBounds::memberOffset`). The table's entry of a member
   function has a leading parameter for `this`.
7. **`field-address` in C++** covers the members with a bound and their
   counts only, and a reference that is not `const`: `std::min(size_, n)`
   binds `const` references and is allowed; `std::swap(size_, other.size_)`
   is not.
8. **Rule W (§5.2) as built.** A change through a `std::shared_ptr` or a
   pointer kept in memory is allowed on an aliased class, on a shareable
   one (a class that locks its members: a borrow of them does not outlive
   the guard), and on a library type with no storage that changes type in
   place, a string counting as such storage here. Two refinements came
   from RFC 0044's correct set: assigning or resetting the `shared_ptr`
   itself (`current_ = std::move(next)`) is not a change through it, and a
   local made by a `fresh` function (`std::make_shared`) that is only
   dereferenced and moved is the function's own until it is published
   (the copy, change and publish of a snapshot).
9. **Rule K** recognizes an `erased` library class (`std::function`,
   `std::any`, `std::packaged_task`; not a thread or a future) as storage
   that changes type, and a polymorphic class that is not aliased held
   through a library template, whether or not the template was
   instantiated yet.
10. **Rule S** ends a writable borrow of a string's characters (a `char *`,
    a `char &`, a `std::span<char>`) the function does not own at a call
    to one of the program's functions, and at a change through a pointer
    kept in memory or a `std::shared_ptr`; a call of the library's does not
    end it. Its error is `stale-borrow`'s, with its own wording
    (`'p' points at the characters of 'name', which the call to 'flush'
    may have moved`).
11. **A write through a stale reference is a use (RFC 0044).** RFC 0044's
    flow skipped the left side of `=`, so `int &c = v[0]; v.push_back(1);
    c = 2;` compiled; only a read was reported. Assigning through a
    reference is now checked as a use, except where the borrow ended by a
    move: a moved-from object may be assigned (X-A5). A bug fix, pinned in
    `test/Safe/cxx-borrows.cpp`.
12. **Safe files (§6)** do not turn on the fast mode: a unit is fast only
    under `-fweavec-safe`, which the guide tells a team to add once a
    unit's files are all converted. Deciding the mode after the unit is
    parsed would reach the driver's macros for the library's hardening.
13. **The survey (§7)** runs a fresh copy of Safe C++'s expression rules,
    the bounds and the flow over each guarded function, and in C the rules
    and ownership's over a copy of the unit's table, so that nothing a
    surveyed function does reaches the safe functions' results or the
    ledger's tiers. The plan does not group `owner-path` by class: the
    error does not carry the class. A function is listed in the plan once,
    by its file, line and name, however many units include it.
14. **The checker does not run after a fatal error.** A unit that stopped
    at a missing header crashed the rules (on main too); they are skipped
    then.
15. **Line limit (D6):** the library is 26,168 lines of the 26,500.
16. **The forty spatial bugs of S0 (e)** were not written: the agent asked
    to write deliberately buggy programs was stopped by a safety
    classifier before it wrote any, and the set was not attempted again in
    another form. G2 is measured on the probes and the tests instead.
17. **Strings keep their guards in C++.** tinyxml2's conversion (G3)
    bounded the parser's cursors `WEAVEC_NULL_TERMINATED`, and RFC 0042's
    conversions scanned the rest of the document at each entry, return and
    store of a cursor: parsing `dream.xml` took 150 times as long. In C++ a
    string is checked at compile time (`string-index`, `string-write`,
    `call-args`) and its accesses keep their guards when it runs: no entry,
    return or store scans it, and a string member gets no field
    annotation. The conversion, unchanged, then parses in 628 units against
    the guarded original's 679.
18. **Globals in a safe file** are safe code's for `thread-share` and the
    audit (they were taken for guarded code's). The survey covers the
    unit's globals and static locals too: tinyxml2's twelve `thread-share`
    errors on globals were missing from its plan in G3 and are counted
    since (the ledger's `surveyGlobals`, added to their file's errors).
19. **An index past a container's `size()` through its `data()`**, now
    accepted in safe code, is caught by the checked build
    (`container-overflow`); in the fast build, which RFC 0045 builds
    without the container annotations, a read inside the capacity reads an
    element of the vector's own type and is not reported, as RFC 0045 says
    of `container-overflow` in general.
20. **From the review of the implementation:** a `new` handed to a smart
    pointer is accepted only when the pointer deletes it as it was made
    (`new T[n]` into `T[]`, `new T` into `T`, a derived object through a
    base only when the base's destructor is virtual); `raw-owner`'s check of
    a raw pointer handed to an owner applies to smart pointers only (a
    `std::string` or a container made from a `char *` copies it, which RFC
    0044 rejected); a constructor may initialize a count to zero without
    its pointer; and a C++ unit's classes' bounds, rule K and the agreement
    of declarations are checked in safe code only, so that a guarded unit
    keeps compiling whatever its classes say, as a C unit does.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset) on
2026-11-15, with the compiler of the final commit unless a row says
otherwise. The evidence is in `build/rfc47/` (not committed): `survey/`
(the four projects' logs before and after, `uniq.py`, `yield.py`,
`newly.py` and their output), `s1/` and `s6/` (the probes), `g3/` (the
two conversions with their agents' `record.md`, the unmodified copies,
`bench/` and `ybench/`), `g4/` (the Rust rewrite and its `record.md`),
`g7/`, `g9/` (`g9.py`, `classify.py`, `why.py` and their output). The
agents had the guides and `--weavec-explain`; the tinyxml2 agent's
compiler embedded the rule pages from before they were rewritten for this
RFC (its `bad-annotation` page still said the bounds were C's only), which
the yaml-cpp agent's did not.

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass.** `test/Safe/cxx-bounds.cpp`, `cxx-aliased.cpp`, `cxx-string-borrow.cpp`, `cxx-casts.cpp`, `safe-file.cpp` (with `Inputs/safe-file.h`), `aliased-c.c`, and new cases in `cxx-borrows.cpp`, `cxx-expressions.cpp`, `cxx-thread-send.cpp`; `test/Guards/cxx-bounds.cpp` (the `pointer-out-of-bounds` lines, an entry from guarded code, a member-counted result, `argv`, in both builds), `ir-cxx-bounds.cpp`, `safe-ended-end.c`; `test/Driver/safe-survey.cpp` (the survey, ledger version 10, the audit's plan, version 6); `LibrarySpecTest.ClassRowsBoundPointers`. `ctest --preset dev` passes 50 of 50; the Linux container's `ci-release` (libstdc++, aarch64) 50 of 50. |
| G2 The probes | **Part.** `slice.cpp` compiles with one annotation (`WEAVEC_ENDED_BY`) where it got six errors, and its accesses are compared with their bounds (an index past a four-byte bound inside a larger array stops). `shared-switch.cpp` stops the compile (`owner-path`). The forty agent-written spatial bugs were not written (amendment 16). |
| G3 Conversions in place | **Pass.** **tinyxml2**: 416 of 444 functions safe (93.7%; the gate asks 70%), `xmltest` passes 517 of 517 in the checked and the fast build with no report, `tinyxml2.h`'s public declarations changed only by annotations; 3 rounds to a clean compile, +333 −215 lines; 3 classes aliased, 58 `NULL_TERMINATED`, 42 `NULL_TERMINATED_OR_NULL`, 6 `COUNTED_BY`, 22 `LIFETIMEBOUND`; 19 functions guarded (the memory pool, placement `new`, `const` getters that normalize in place, `va_start`, a `const_cast`, two clones a false `stale-borrow` stopped). **yaml-cpp**: 2,454 of 2,474 functions safe (99%; the gate asks 80%), its 1,008 tests pass in the checked and the fast build, `test/` and the CMake files untouched; 18 rounds (the library clean at round 12), +387 −134 lines, 73 files marked, 5 classes aliased (the node graph), 29 `COUNTED_BY`; 15 functions guarded (inserting `get`s with a `const` twin, lazy `const` state, the `void *` graph builder). Rejections the agents judged false: tinyxml2 about 10 (results taken to point into every argument before `LIFETIMEBOUND`; a clone taken as a borrow of its source), yaml-cpp 32 (8 the `const`-twin rule on inserting `get`s, 13 `void *` handles, 3 a list iterator across erasing another element, the rest single). Two gaps of the workflow: a header the plan calls ready can fail in a unit that instantiates its templates, or compiles it at another standard (yaml-cpp's `convert<std::string_view>` is compiled only under C++17); and the plan does not count globals' errors. |
| G4 Against a rewrite | **Fail.** From the same start, the tinyxml2 conversion took 1,201 s, the Rust rewrite 1,145 s (2,824 lines of library, 2,118 of tests, all 517 checks ported and passing, 4 rounds): 1.05 times, where the gate asks at most 0.5. |
| G5 Cost | **Fail at the 1.3 bound; pass against guarded.** tinyxml2's `dream.xml` parse (best of five): Clang 364, the original guarded 679, the conversion checked 705, fast 628 (1.73 times Clang); the Rust port measures about 0.28 to 0.35 ms a parse against Clang's 0.36. Before amendment 17 the conversion took 103,466 (150 times). yaml-cpp, a parse and emit of a 160,000-line document (best of five): Clang 577 ms, guarded 2,254 ms, the conversion fast 1,205 ms (2.09 times Clang). No converted unit is slower than it was guarded. |
| G6 Defects found | **One memory error, reproduced.** yaml-cpp's regex matcher reads one byte past a caller's buffer handed to `Emitter::Write(const char *, std::size_t)`: the unmodified library under the guards stops with `heap-buffer-overflow at src/stringsource.h:19:49`, and AddressSanitizer agrees; the conversion's `index-single` asked for the bound that found it. Recorded from the agents, not reproduced: `Binary::data()` of an empty owned `Binary` indexes an empty vector (a hardened-library assertion); tinyxml2's bool-serialization globals and its `const` getters that normalize in place race between threads (`thread-share`, `owner-path`); a dead test `if (length == 0)` in `GetCharacterRef` (by reading). |
| G7 What was accepted before | **Pass.** RFC 0044's 110 seeded bugs: 81 stop (81 before). Its fixed twins rejected: 22 (23 before; 4 are accepted now that arrays and pointers have bounds, 3 newly rejected by rule W). Its correct files rejected: 5 (5 before; one accepted, one newly rejected by W). Each of the four newly rejected compiles with one `WEAVEC_ALIASED`. RFC 0046's forty correct programs: none newly rejected; one fixed twin accepted. The LRU, `json` and indexer ports compile as before. |
| G8 The survey again | **Part.** The four projects, unedited: 2,275 distinct errors become 1,737; none of the 184 for an array used as a pointer remains; `index-single` 1,204 to 811, `owner-path` through a pointer kept in memory 447 to 284, through a `std::shared_ptr` 0 to 69 (yaml-cpp, rule W). `call-args` rises from 0 to 31, the library's pointer-and-count arguments without a bound, which the gate does not allow. |
| G9 Nothing elsewhere | **Pass but `main`.** 2,038 compiles succeed with main's compiler and this one (RFC 0044's and RFC 0046's sets, `test/cases`, `test/Guards`, C and C++, guarded, fast and checked); 1,843 give the same object. Each of the 195 that differ uses something new: 192 are the safe builds of the 96 units whose `main` takes `argv`, now counted by `argc` and checked (amendment 2); two are `test/Guards/safe-ended-end.c` (amendment 4); one is the guarded build of `test/Guards/cxx-bounds.cpp`, whose classes declare bounds. 34 compiles succeed with one compiler only: RFC 0044's files of G7, the new tests, and three cases that index a container's `data()` (`test/Guards/container-overflow.cpp`, `test/cases/detection/cxx/container-data.cpp`, `deque-past-end.cpp`), now accepted (amendment 19). |
| H1 Hygiene | **Pass.** Library 26,168 of 26,500 lines; the runtime unchanged at 10,583. |

What the numbers say about the owner's goal: an existing C++ library
converts in place. Two projects WeaveC was never tuned on reached 94% and
99% of their functions safe, with their own test suites passing and their
public headers unchanged, in about twenty minutes of an agent's time each,
and one conversion found a real out-of-bounds read the guarded build
confirms. What they do not say: that a conversion is cheaper than a rewrite.
For a 5,400-line library the Rust agent finished as fast, and its result
runs faster; the conversion's case rests on what the gate did not measure,
a project too large or too entangled to rewrite in an afternoon, and on
the build, the callers and the tests staying as they were.
