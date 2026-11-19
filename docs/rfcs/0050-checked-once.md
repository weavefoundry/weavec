# RFC 0050: Checked once

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-18
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Carries out what
  [RFC 0048](0048-safe-code-without-the-shadow.md) accepted and did not
  build (its §6.1, §6.2 and the paths of its §8; its amendments 8 and 12)
  and answers its failed gates G5, G7 and G8, which
  [RFC 0049](0049-errors-that-carry-their-fix.md) left as _Future work_.
  Supersedes (Y3) of the guarantees for views, the clause of
  [RFC 0045](0045-type-stable-memory.md) §5 that keeps a function with a
  view parameter from being trusted, and, for the fast mode only, the
  shadow of a pooled slot ((Y1); RFC 0045 §3). Extends RFC 0048's
  `call-args` case for C library calls from safe C++ to Safe C, where
  [RFC 0042](0042-typed-memory.md) left it to a check when the program
  runs. Adds no annotation, no rule identifier and no report kind. The
  runtime's ABI becomes 46.

This RFC was written at the request of the project owner on 2026-11-18,
who asked a tenth time for the single change that would do most to make
an AI rewrite into C or C++ as attractive as an AI rewrite into Rust, to
be implemented next, with breaking changes and the removal of old code
welcome. The measurements in _Motivation_ were taken on main at
`bffda036` (RFC 0049 merged), darwin-arm64, the `dev` build, Homebrew LLVM
23.1.0, and from the records RFC 0048 and RFC 0049 left in `build/rfc48/`
and `build/rfc49/`. The scripts, the samples and the probes are in
`build/rfc50/` (not committed; `README.txt` there says what each file
is). Nothing of the design has been built or prototyped; stage S0 is
where it meets code, and S0 can stop it.

The owner accepted the design as drafted on 2026-11-18, before any of it
was built, and with it the six decisions that were theirs to make. They
are marked **(D1)** to **(D6)** where they are argued, and collected
here. The thresholds of §7 and the _Unresolved questions_ are still open,
and S0 is where they are settled; by (D5), what it measures may stop the
work, and a second weakening of the shadow for pooled arrays (§3.1) is
not covered by (D3) and comes back to the owner.

- **(D1) The cost of a fast safe program is the gate, and the target is
  Rust's.** New safe code within 1.15 times Clang's build of the same
  source, a conversion within 1.25 to 1.3, Safe C within 1.5. RFC 0047 and RFC
  0048 each set a cost gate, missed it and were merged. This one is not
  Implemented until its cost gates pass or the owner changes them.
- **(D2) A view is trusted as an owner is.** In a fast unit an element
  read through `std::span`, `std::string_view` or the hidden iterator of
  a range-based `for` is checked by the library's own comparison and by
  nothing else, and a library function may take a view and still be
  trusted. This is (D6) of RFC 0048, built, and one step further.
- **(D3) A pooled slot keeps its shadow.** In a fast program a typed
  block's release and its reuse write no shadow. Code that is not safe
  then reads a released pooled block as the zeroed object of its type
  that it is, where today it stops until the block is reused. The checked
  mode does not change. This is the one place this RFC makes a fast
  program detect less.
- **(D4) Safe C rejects when it compiles what it stops when it runs.** A
  C library call that accesses more through a pointer to one object than
  the object has is `call-args` in Safe C, as it has been in safe C++
  since RFC 0048. Existing Safe C that relied on the run-time check does
  not compile until the pointer has a bound; the error carries the fix.
- **(D5) S0 decides before anything is built.** Each part is first
  switched off by a throwaway option and timed. If the json port is not
  within 1.25 times Clang with both switched off, the design is wrong
  about where the time goes; the work stops and the numbers are reported.
- **(D6) The library's line limit rises from 31,490 to 31,850; the
  runtime's stays at 11,000.** Nothing this RFC removes pays for what it
  adds.

## Summary

Nine RFCs gave C and C++ rules an agent can iterate on. For new code the
loop is no longer behind Rust's: yesterday a fresh agent ported Python's
`json` to safe C++ in two rounds and five minutes, where RFC 0044's Rust
agent took one round and seven and a half. Every function of the port is
safe. It runs at 1.55 times Clang's build of the same source, and RFC
0044's Rust port runs the same benchmark in two thirds of Clang's time.
The agent is finished in minutes and the program is slower for as long
as it exists. That column failed its gate in RFC 0047 and again in RFC
0048, and RFC 0049 did not work on it.

The cost is not spread thin. Sampled, three fifths of it is the runtime's
allocator, which does for every typed block the work the shadow needs and
a fast program's safe code never reads. Most of the rest is one line,
`text_[idx]` on a `std::string_view`: the library compares the index with
the size, as Rust would, and then WeaveC reads the shadow for the same
byte. The idiom a model writes first in modern C++ is the one checked
twice.

This RFC states the principle the fast mode was built toward and applies
it where it does not hold yet. **In a fast safe unit a fact about memory
is checked once, by the cheapest thing that can check it, as early as it
can be checked**: by a rule when the unit compiles; else by one
comparison with a bound the code holds, the library's or the compiler's;
by the shadow only where a pointer arrives from code that is not safe.

- **Views** (D2). The members of the library's views whose precondition
  its hardening checks are trusted, a trusted function may take a view,
  and a range-based `for` reads its elements without a guard. A view is
  compared with its pointer's bound where it is made.
- **The allocator** (D3). A typed block's allocation and release touch
  the thread's pool, the slot's word and the object's bytes. No shadow,
  no second look-up, one counter.
- **Safe C** (D4). A sized library call on a pointer to one object is a
  compile error with its fix, not a stop in a test.

Expected result (gates, §7): the json port at or under 1.15 times Clang
from 1.55, tinyxml2 at or under 1.25 from 1.44, yaml-cpp at or under 1.30
from 1.58, cJSON at or under 1.5 from 1.99; a typed allocation and its
release no slower than the system allocator's, from 1.56 times; nothing
the fast mode believes shown wrong by `verify`.

## Motivation

### The loop caught up and the result did not

| | Agent time | Rounds | Run time, times Clang |
| --- | --- | --- | --- |
| `json` port, Rust (RFC 0044 G4) | 7 min 29 s | 1 | 0.64 of the C++ port's Clang build |
| `json` port, safe C++ (RFC 0049 G7) | 5 min 1 s | 2 | 1.55 |
| tinyxml2 rewritten in Rust (RFC 0047 G4) | 1,145 s | 4 | 0.8 to 1.0 |
| tinyxml2 converted in place (RFC 0049 G4) | 1,015 s | 17 | 1.44 (RFC 0048's conversion) |
| yaml-cpp converted in place (RFC 0049 G4) | 1,341 s | 21 | 1.58 (RFC 0048's conversion) |
| libzmq, 50,000 lines, 81% of functions safe (RFC 0049 G5) | 73 min | about 80 | not measured |

The first two rows are the case the owner's question is about when the
code does not exist yet: an agent writes a program in a language whose
compiler rejects memory errors. Safe C++ now takes less of the agent's
time than Rust did. The last row is the case when the code exists: no
agent rewrites libzmq in Rust in 73 minutes, and its tests pass
throughout. On both, what the choice gives away is the last column.

The history of that column:

| Gate | Asked | Measured |
| --- | --- | --- |
| RFC 0045 G5, the `json` port | 1.9 | 1.73 (passed; Safe C's half was not met) |
| RFC 0047 G5, tinyxml2 and yaml-cpp | 1.3 | 1.73 and 2.09 |
| RFC 0048 G5, tinyxml2, yaml-cpp, `json`, LRU | 1.25, 1.4, 1.4, 1.15 | 1.44, 1.58, 1.72, 1.24 |
| RFC 0048 G7, guards left in tinyxml2 | 38 | 271 |
| RFC 0048 G8, the typed allocator against the system's | 1.5 | 1.56 and 1.80 |

RFC 0049 called what is left "engineering that has been named twice and
needs no decision" and went to the fixer. Its own gates then said where
an agent's time goes on a conversion: to what no annotation fixes, which
an agent rewrites well and a compiler does not. What a compiler can do
and an agent cannot is make the result fast. And two of the three parts
below do need a decision, which is why they were deferred twice.

### Measured: new code, all of it safe

`build/rfc50/json/` builds RFC 0049's G7 port (820 lines of library; 94
functions with its program, all safe, no annotation) and runs its
benchmark (`bench
20000`: 20 rounds of `dumps` and `loads` over 20,000 objects). Median of
seven interleaved runs:

| Build | ms | Times Clang | Peak memory |
| --- | ---: | ---: | ---: |
| `clang++ -O2` | 692 | | 119 MiB |
| `weavec-c++ -O2 -fweavec-safe` (fast) | 1,069 | 1.55 | 158 MiB |
| `-fweavec-safe-checks=full` (checked) | 2,367 | 3.42 | |
| `weavec-c++ -O2`, no safe function (guarded) | 2,366 | 3.42 | |
| RFC 0044's Rust port, `--release` | 442 | 0.64 | |

(The Rust port is another agent's program for the same brief, so 0.64 is
partly the two programs. Clang's build with libc++'s hardening at its
`extensive` level runs in the same time, 0.65 s against 0.68 s in three
runs each: the library's own checks are not the cost.)

Both builds sampled over the same benchmark (`sample-j-fast.txt`,
`sample-j-clang.txt`), each function's samples as a share of the Clang
build's whole run:

| Where | Clang | Fast | Difference |
| --- | ---: | ---: | ---: |
| The allocator and zeroing | 0.30 | 0.65 | +0.35 |
| `Scanner::scan`, `scan_string`, `Encoder::encode` | 0.11 | 0.27 | +0.16 |
| A `std::vector<Value>` growing | 0.07 | 0.14 | +0.08 |
| Everything else | 0.52 | 0.49 | |
| Total | 1.00 | 1.55 | +0.55 |

The allocator is 63% of the overhead. In the fast build it is `release`
(274 samples), `weavecRtAllocTyped` (251), `weavecRtReleaseForm` (96),
`weavecRtShadowObject` (73), `shadowLive` (64), `deleteAt` (43) and the
zeroing (84); in Clang's it is the system allocator's `_xzm_free` and
`_xzm_xzone_malloc_tiny`, which is itself an allocator that keeps types
apart.

### Measured: the idiom is checked twice

The fast ledger of `json.cpp` has 1,579 accesses, 193 of them guarded
(`guards.py`): 107 in library code that is not trusted (`std::sort`'s
iterators, the hash table's `find` with a `std::string_view` key,
`from_chars`), 14 ranges, 2 scans, and 70 `typed-unbounded` in the
port's own functions. Of those 70, 34 are the load of one byte through a
view's index, and they are the scanner's and the encoder's inner loops:

```cpp
while (idx < text_.size() && is_whitespace(text_[idx]))   // json.cpp:98
switch (text_[idx]) {                                      // :109
c = text_[at];                                             // :171
```

`text_` is a `std::string_view`. `build/rfc50/probe/view.cpp` isolates
it:

```cpp
std::size_t Scanner::digits(std::size_t idx) const {
  const std::size_t length = text_.size();
  while (idx < length && text_[idx] >= '0' && text_[idx] <= '9')
    ++idx;
  return idx;
}
```

Compiled fast, each turn of the loop compares `idx` with the size (the
loop's test and the library's assertion, which the optimizer makes one),
then shifts the byte's address, loads its shadow, compares it with the
shadow of the view's first byte and branches, and then loads the byte.
The same file's `std::span<const long>` index and its `for (long x : v)`
over a vector keep the same guard. Its `v[i]` on a `std::vector` has
none: RFC 0045 trusted the owners' members and no view's.

The guarantees page says so, as (Y3): "an access through an iterator, a
`std::string_view`, a `std::span` ... is guarded." RFC 0048 decided
otherwise (its (D6), §6.1 and §6.2), was accepted, and did not build it:
its amendment 8 says "the guards left in the two ports' and the two
conversions' safe code are not there". In the port an agent wrote
yesterday, they are in the scanner's inner loop. A model asked for safe
C++ writes `std::string_view` and `std::span` because they are the safe
forms of a pointer and a length, and those are the two forms the fast
mode does not believe.

The hash table is the same decision from the other side. `Object::find`
looks a `std::string_view` up in a `std::unordered_map` with string
keys. `emplace` with a `std::string` runs as a trusted clone; `find`
with a view is guarded code, because RFC 0045 trusts no function with a
view among its parameters.

### Measured: the conversions

RFC 0048's records, which RFC 0049 did not change (it did not touch the
passes or the runtime): tinyxml2's fast ledger keeps 271 guards, 103 of
them the range guards of loops in safe functions and 145 in guarded
functions and their instantiations; yaml-cpp spends 178 ms in the runtime
where Clang's build spends 99 ms in the system allocator; a typed
allocation and its release cost 26.6 ns against 17.1. RFC 0045's cJSON
conversion, the slowest Safe C at 1.99, has 38% of its samples in the
runtime.

### Measured: Safe C compiles what it then stops

The agent that converted nng (RFC 0049 G5; 65,000 lines of C) wrote of
what cost it the most time: "Unbounded pointers to sized library calls
compile and then trap ... 8 rounds of run-time whack-a-mole ... I
expected these to be `call-args` or `index-single` errors (or a fix) when
the code compiles." `build/rfc50/probe/lib.c`:

```c
WEAVEC_SAFE size_t copy(char *dst, const char *src, size_t n) {
  memcpy(dst, src, n);
  return n;
}
```

As C it compiles, and stops when a test calls it with eight bytes:
`pointer-out-of-bounds at lib.c:5:65: write of 8 bytes`. The same file
as safe C++ does not compile, and says what to write:

```text
lib.cpp:5:72: error: 'dst' points to one object, and 'memcpy' may access more than its 1 byte through it [weavec-safe:call-args]
lib.cpp:5:31: note: fix (likely): declare it 'char *WEAVEC_COUNTED_BY(n) dst'; 'n' is its count in the call to 'memcpy' at line 5
```

The rule, its evidence and its fix are in code both languages share
(`libraryAccess` in `SafeCheck.cpp`), behind a test for C++. The check
that could be made once, by the compiler, in a fifth of a second, is made
by a test run, if a test reaches the line.

### Why this, and not the fixes that rewrite

RFC 0049's closing paragraph named its successor: fixes that rewrite
owners, `const` twins and in-place strings. This RFC does not take that
advice, for three reasons its own gates give.

- The fixer agreed with people on every declaration both annotated
  (G3), and did not make a conversion faster (G4): agents spent their
  time on design and on reading the tool's edits back. A rewrite the
  compiler proposes is a larger edit to read back, in the part of the
  work agents already do well.
- The largest numbers RFC 0049 measured are not about the fixer. An
  agent converted 81% of libzmq's functions in 73 minutes with its tests
  passing. Conversion in place wins on time at the size where a rewrite
  is not an afternoon. What it cannot answer is "and how fast is it".
- New code needs no conversion at all, and its loop is ahead. Its cost
  is the whole of what stands between "write it in safe C++" and "write
  it in Rust", and it is two mechanisms.

## Soundness

### What is believed that was not

- **(V7) A view's checked element.** In a fast unit, an element a safe
  function reads or writes through a trusted member of a library view,
  after the library compared the index with the view's count, is inside
  memory of the element's type. This follows from (V4) of RFC 0048, "a
  `std::span`, a `std::string_view` and the standard containers hold a
  pointer and a count that satisfy (V2)", which is stated there and not
  used: (V5) lists "through a view's checked index" among the accesses
  with no guard, and the guard stayed.

  What keeps (V4), each a rule or a check that exists unless marked new:
  a view made from an owner or an array takes the owner's count (the
  library's constructors, trusted); a view made from a pointer and a
  count is compared with the pointer's bound where it is made (**new**,
  §2.3; RFC 0048 §6.1 specified it); a view that arrives from code that
  is not safe is checked at the entry or where a call returns it
  (**new**, §2.3); a view does not outlive what it points into
  (`stale-borrow`, `escape`, `call-alias`; (X3), (X4)); and where those
  rules stop following (a view in a member of an aliased class, (X6)),
  the view points into a block that was released to the pool of its
  element type and class, or reused for it on the same boundaries, so
  its count still covers elements of its type ((Y1), (Y2)). A view into
  storage that changes type in place (a short string's characters) rests
  on the rules alone, as (Y6) already says of a borrow.
- **(V8) A view given to the library.** A trusted library function that
  takes a view reads and writes inside it. This is (V4) again, plus what
  RFC 0045 §5 already assumes of the library's code: that it is correct
  when its preconditions hold.
- **(V9) A range-based `for`.** The element the hidden iterator reads is
  inside the range: the loop compares with the end before each read, and
  `stale-borrow` rejects a change to the range in the loop's body. As
  RFC 0048 §6.2 says, a change through an aliased object the rules do not
  follow leaves the loop in the old buffer, which is memory of the
  element's type to its old end.

### What gets weaker, in plain terms

- **(D3) Code that is not safe, in a fast program, no longer stops on a
  pooled block between its release and its reuse.** Today a typed block
  that safe code releases is marked released in the shadow until an
  allocation of its type takes it again, which with a pool that hands
  out the last slot first is usually the next allocation. In that
  interval a guarded function that reads it stops with
  `heap-use-after-free`, and a validating entry given a pointer to it
  stops. After this RFC the slot's shadow stays that of the object it
  held, so both read the zeroed object, as safe code has since RFC 0045.
  A second release still stops (the slot's word says released). Nothing
  that safe code believes changes: (Y2) never rested on that interval.
  The checked mode marks as today, and so does a program with a `verify`
  unit (§4).
- **A view's element is checked by the library alone.** If the library's
  check is wrong, or switched off, nothing stands behind it. The driver
  sets `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` and
  `_GLIBCXX_ASSERTIONS` unless the command line chooses; §2.5 makes a
  fast unit whose command line chooses less a unit that trusts nothing.
  Under `full` every access keeps its guard.
- **A fast Safe C unit may stop placing the reference test** ((O4); §5.2,
  only if S0 finds it costs). A reference to a block that code which is
  not safe released is then read, as (M3) says it may be: the block's
  bytes stay, and nothing it refers to is reused. Fast safe C++ has
  placed none since RFC 0048's amendment 11.

### Bugs caught

At compile time, in Safe C, what was caught when the program ran (D4):

```c
WEAVEC_SAFE void put(struct buf *b, const char *src, size_t n) {
  memcpy(b->bytes, src, n);     /* call-args: 'src' points to one object */
}
```

At a view's making, what the guard of each later access caught:

```cpp
std::string_view tail(const char *WEAVEC_COUNTED_BY(n) p, std::size_t n) {
  return std::string_view(p, n + 1);          // pointer-out-of-bounds, here
}
```

### Bugs deliberately not caught

Nothing safe code did is newly missed. What (D3) gives up is a stop in
code that is not safe, described above. A bad comparator given to
`std::sort`, an iterator held in a variable and stepped past its end, and
a raw pointer given to a library function keep their guards (§2.4), as
RFC 0048 §6.3 left them.

### Accepted rejections and false traps

- **Safe C that calls `memcpy`, `memcmp`, `strncpy`, `strchr` and their
  kin on a plain pointer with a count that is not a constant within the
  object does not compile** (D4). It stopped whenever the count was more
  than the object, so the code that newly fails to compile and worked
  before is code whose count was always small. The fix is the bound, and
  RFC 0049's evidence E1 proposes it from the call.
- **A view made from a pointer whose bound is smaller than the count
  stops where it is made**, not at the first access outside. A view made
  one too long and never read to its end stops now and did not before.
- No correct program stops that did not, otherwise.

### Assumptions

None is added. (Y-A4) of RFC 0045, that the standard library's owner
types keep their invariants given the preconditions their hardening
checks, now covers its views and the functions that take them. T-A1 and
V-A1 (code that is not safe keeps typed memory) now cover a view such
code writes into a member that safe code loads.

## Detailed design

### 1. The principle

For a fast safe unit, each fact has one place where it is established:

| Fact | Established by | Not again by |
| --- | --- | --- |
| A pointer points to an object of its type | The rules; pools by type (Y1) | The shadow |
| An index is within a bound the function holds | One comparison | The shadow |
| An index is within a library owner or view | The library's comparison | The shadow |
| A view covers its elements | A comparison where it is made | A guard at each access |
| A string's current byte is before its terminator | `string-index`; a check where it is stepped | The shadow |
| A library call stays inside what it is given | `call-args` when it compiles; else a comparison | A checked wrapper |
| A pointer from code that is not safe is what it says | The shadow, at the entry | |
| A block is released once | The rules; the slot's word | The shadow |

Rows 1, 2, 5 and 7 are RFC 0042, RFC 0045 and RFC 0048 as built. Rows 3
and 4 are §2. Row 6 is §5. Row 8 is §3. A new guard in a fast safe
function is a row someone has to add to this table.

### 2. Views and the library (D2)

#### 2.1 Trusted members of a view

RFC 0045 §5 trusts a function of namespace `std` with a name the standard
does not reserve, of a class that is no view, none of whose parameters is
a pointer, a view or a callable. The clause "of a class that is no view"
goes. A member of a class the kinds call a library view
(`CxxKinds::viewClass`: structural, or by `gsl::Pointer`) is trusted when
it is not an iterator's and takes no pointer, iterator or callable:
`operator[]`, `front`, `back`, `first`, `last`, `subspan`, `substr`,
`remove_prefix`, `remove_suffix`, `size`, `length`, `empty`, `data`,
`begin`, `end`, the comparisons, and `std::string_view`'s `find` family,
`compare`, `starts_with` and `ends_with` with a view or a character.

A member whose precondition the library does not check in the mode the
driver sets stays out by a table row, `class std::span :
unchecked(<member>, ...)`; S0 (c) reads each member in libc++'s `fast`
mode and under `_GLIBCXX_ASSERTIONS` and writes the rows. `data` is
trusted and its result is a plain pointer with the view's count as its
bound where the function holds the view, as a container's `data()` has
been since RFC 0047.

The mechanism is the one `std::vector::operator[]` has: the call site is
marked by `TrustedCalls`, the member's body runs as a `.weavec.t` clone
or inlined from one, and its accesses are `trusted` in the ledger.

#### 2.2 A view as a parameter

A trusted function may take a view by value or by reference:
`std::string::append(std::string_view)`, the constructor of a
`std::string` from one, `std::unordered_map::find` with a transparent
hash, `operator==` on two views, `std::hash<std::string_view>`. The
clause "none of whose parameters is ... a view" goes for library views;
a pointer, an iterator or a callable among the parameters still keeps
the function guarded. A class of the project's that is a view (a
`WEAVEC_BORROWED` member) is not a library view and changes nothing
here.

#### 2.3 Where a view is made

- **From an owner, an array or a literal**, by the library's
  constructors and conversions: trusted, as their owners' members are.
- **From a pointer and a count, or two pointers** (`std::span(p, n)`,
  `std::string_view(p, n)`, `std::span(first, last)`, the table's
  `counted` and `ended` clauses): compared with the pointer's bound where
  the function holds one (`pointer-out-of-bounds` when the program runs);
  `call-args` when the pointer points to one object and the count is not
  a constant of at most one, which RFC 0048 (D2) already reports for a
  call; guarded once, as a range, when the pointer's bound is not known.
- **From a string** (`std::string_view(s)` for a
  `WEAVEC_NULL_TERMINATED` pointer): the library's `strlen`, which the
  string's declaration covers.
- **At a validating entry**: a parameter that is a library view taken by
  value gets one range guard over its elements, as a bounded pointer
  does. A view behind a reference is believed, as references are (Y-A1).
- **As the result of a project function that is not safe**: one range
  guard where it arrives, as a plain pointer's result gets.

#### 2.4 The element of a range-based `for`

As RFC 0048 §6.2: the dereference, the step and the comparison of the
hidden iterator of a range-based `for` over an array, a library
container, a `std::span` or a string view are trusted call sites. The
range expression must be one the function holds for the loop: a local, a
parameter, a member of `this`, or a temporary the loop extends.

What keeps its guard: an iterator in a variable (`*it`, `it->second`,
`it + n`), the library's algorithms and every other library function that
takes an iterator, a raw pointer or a callable (`std::sort`,
`std::from_chars`), and a `data()` result that is stepped. S0 (a) records
how much of each program's time that is; it is _Future work_ here.

#### 2.5 A unit that chooses its own hardening

`wantsLibraryChecks` sets the hardening macros unless the command line
defines them. A fast unit whose command line sets
`_LIBCPP_HARDENING_MODE` to `_LIBCPP_HARDENING_MODE_NONE`, or undefines
`_GLIBCXX_ASSERTIONS`, has no trusted library function and no trusted
call site: the driver passes the choice to the action, and `TrustedCalls`
marks nothing. (S0 (c) establishes what happens today; if a unit can
switch the library's checks off and keep its trusted functions, that is
a hole of RFC 0045's and is closed here.)

### 3. The allocator's typed path (D3)

(P1) to (P3) of RFC 0045 do not change: which blocks are typed, which
pool a block returns to, and that a slot leaves its pool only through the
quarantine and a sweep. What changes is what the two operations execute.

#### 3.1 A pooled slot keeps its shadow

Today `poolRelease` zeroes the object, marks the slot's granules released
(`shadowDead`) and pushes; `poolAlloc` pops and `makeLive` writes the
slot's word and the object's shadow with its color (`shadowLive`,
`weavecRtShadowObject`). In a fast program:

- a release into a pool zeroes the object, writes the slot's word
  (released, with the size it had) and pushes. No shadow.
- an allocation from a pool pops and writes the slot's word. It writes
  shadow only when its size differs from the size the word recorded,
  which for `new T` and for a `malloc(sizeof(T))` is never.
- a slot's color is the one it got when it first entered the pool.
- a slot that leaves its pool (a release by code that is not safe, the
  pool's budget, a sweep) is marked released then, as today.

The slot's word still says whether the block is live, so a second
release, a release of an address inside a block and a release by the
wrong form are reported as today.

Arrays (`new char[n]`, a vector's buffer) share a size class and differ
in `n`, so they write shadow at most allocations. S0 (b) measures that
case. If it is what keeps G5 from passing, the alternative is a pooled
array slot whose whole class size stays addressable, which makes guarded
code's check of such a block coarser by up to the class's rounding. That
is a second weakening and is **not** covered by (D3): it comes back to
the owner with S0's numbers.

The checked mode is not touched: a `full` unit's typed blocks go through
the quarantine (Y5) and never enter a pool. A program in which any unit
was compiled `-fweavec-checks=verify` marks as today, so that the
monitors of believed accesses read a shadow that says released; the unit
says so through the entry that registers it.

#### 3.2 One short path each way

The typed entry points (`__weavec_rt_new_typed`, the typed `malloc`, the
sized and typed releases) take, when the site's record names its pool and
the thread's cache entry holds that pool:

- **allocate**: pop the entry's last slot; write the word; return. The
  bytes are zero from the release.
- **release**: read the word and compare (live, this form, this size);
  write it released; zero the object (inline to 128 bytes, as now); push.

Everything else (an empty or full entry, another pool in the entry, a
slot not at its start, a huge block, a thread inside the allocator)
falls to today's path. The statistics (`WeavecRtStatAllocations`,
`Typed`, `Releases`, `Pooled`) and the two byte deltas become one count
per entry, folded when the entry is refilled or flushed. Whether the
release's claim stays a compare-and-swap is S0 (b)'s to measure: two
threads releasing one block is a race the rules reject in safe code
(RFC 0046), and the word's test still stops the second of two releases
in turn.

The entry points stay calls. An inline pop in the unit's IR is built
only if S0 (b) shows the calls cannot meet G5.

#### 3.3 Memory

The fast build's peak is 158 MiB against Clang's 119 (1.33 times). S0
(b) records where the difference is (the pools' slot lists, class
rounding, slots kept for a type that no longer allocates) before
anything is changed; G2 sets the bound.

### 4. Verify mode and the ledger

Every access that loses its guard by §2 is a `trusted` row, and under
`-fweavec-checks=verify` keeps its guard as a monitor that reports
`weavec.proven`, as the owners' trusted accesses do. The range-based
`for`'s element is monitored the same way. No rule of this RFC removes a
guard outside a safe function or a trusted library function. The ledger
stays at version 12 and the audit at 8: no field is added, and the
summary line's count of trusted library functions rises.

### 5. Safe C (D4)

#### 5.1 Library calls on a pointer to one object

`libraryAccess` runs for Safe C as for safe C++: a call to a function of
the library table whose row says it accesses, through an argument that
points to one object (W1), more bytes than the object has, is
`call-args`, with RFC 0049's fix from the row's count (E1). A count that
is a constant within the object (`memset(p, 0, sizeof *p)`,
`memcpy(&a, &b, sizeof a)`) is accepted, as it is in C++. A string
function given a plain `char *` is rejected with the note that proposes
`WEAVEC_NULL_TERMINATED`. The run-time comparison stays for the cases
the compiler cannot decide (a pointer with a bound and a count it cannot
compare).

S0 (d) counts what this newly rejects in `test/cases/safe/`, the
documentation's examples and RFC 0041's to RFC 0043's five conversions,
and how many of those errors the fixer clears by itself.

#### 5.2 The reference test, in the fast mode

Fast safe C++ places no reference test (RFC 0048, amendment 11); Safe C
places it in every mode. If S0 (e) finds it is at least 3% of cJSON's or
sds's time, a fast Safe C unit stops placing it, (O4) becomes the checked
mode's in both languages, and _What gets weaker_ above applies. If not,
nothing changes.

### 6. Cleanups and breaking changes

- **Breaking:** `call-args` in Safe C for library calls (D4); a
  comparison, or `call-args`, where a view is made from a pointer and a
  count; one range guard for a view at a validating entry.
- **Breaking:** in a fast program, code that is not safe does not stop
  on a pooled block between release and reuse (D3). `WEAVEC_RT_ABI` 46.
- **Removed:** (Y3)'s sentence that an access through a
  `std::string_view` or a `std::span` is guarded; the two clauses of
  `TrustedCalls` about views; `shadowDead` and `shadowLive` on the pooled
  path of a fast program; the per-operation statistics on that path; the
  test for C++ around `libraryAccess`.
- **Not changed:** a unit with no safe function (it compiles to the same
  object but for the ABI number); the checked mode; the rules of
  ownership, of threads and of the fixer; the ledger's and the audit's
  formats; `weavec.h`.

### 7. Stages and gates

- **S0 Measure first (D5).** Nothing below is built until these are
  written down in `build/rfc50/`.
  (a) Views: a throwaway `-mllvm` option that drops the guard of a
  library view's member access and of a range-based `for`'s element in
  safe functions, and trusts functions with view parameters. The json
  port, the LRU port, tinyxml2 and yaml-cpp timed with it. What is left
  guarded, by line.
  (b) The allocator: a runtime built with the pooled path's shadow
  writes, statistics and second look-up compiled out, and one with the
  claim made a plain store. The pairs benchmark (`build/rfc48/alloc`),
  the json port, yaml-cpp. The share of typed allocations whose size
  differs from the slot's last. Where the 39 MiB are.
  (c) Each member of `std::span` and `std::basic_string_view` in
  libc++'s `fast` mode and libstdc++'s assertions: checked, no
  precondition, or unchecked. What a unit that sets its own hardening
  mode trusts today.
  (d) §5.1 run over Safe C's tests, cases, examples and conversions.
  (e) The reference test's share of cJSON and sds.
  (f) Why a growing `std::vector<Value>` costs twice Clang's in trusted
  clones (0.08 of the json port's 0.55): the relocation's `memmove`
  ranges, a clone that is not inlined, or the allocator again.
  **The decision:** with (a) and (b) together the json port is within
  1.25 times Clang, or the work stops here and this RFC is amended or
  withdrawn with the numbers.
- **S1** Views: §2.1, §2.2, §2.5 and the table's `unchecked` clause.
  **S2** §2.3 and §2.4. **S3** The allocator, §3. **S4** Safe C, §5.
  **S5** The guarantees page ((Y1), (Y3), (V7) to (V9), (O4)), the
  guides' "Fast and checked builds", the rules' page for `call-args`.
  **S6** The gates.

Gates (thresholds are proposals until S0; G2 to G4 are (D1)):

- **G1 Tests.** `test/Guards/ir-views.cpp` (no shadow load at a
  `std::string_view`'s and a `std::span`'s index, at `front` and
  `substr`, in a range-based `for`; the guard kept for an iterator in a
  variable and for a unit that sets `_LIBCPP_HARDENING_MODE_NONE`; the
  monitors under `verify`), `view-made.cpp` (the comparison where a view
  is made, the entry's range guard, their report lines),
  `test/Safe/cxx-views.cpp` (`call-args` for a view from a pointer to
  one object), `test/Safe/typed-rules.c` (§5.1, with its fix),
  `test/Guards/typed-reuse.cpp` (a pooled slot's shadow after release in
  fast, and marked in `full` and with a `verify` unit); `rt_test`
  `pool-keeps-shadow`, `pool-second-release`; one unit test per new
  table row.
- **G2 New code.** Fast, times Clang's build of the same source, median
  of seven interleaved: the json port at most 1.15 (1.55 today), the LRU
  port at most 1.10 (1.24). Peak memory at most 1.15 times Clang's (1.33
  for the json port). Recorded beside them: RFC 0044's Rust ports.
- **G3 The conversions.** The conversions RFC 0048's G5 timed
  (`build/rfc48/g5/g5.sh`), fast: tinyxml2 at most 1.25 (1.44), yaml-cpp
  at most 1.30 (1.58). RFC 0049's G4 conversions recorded beside them.
- **G4 Safe C.** User time against Clang's: cJSON at most 1.5 (1.99),
  sds at most 1.3 (1.63), inih at most 1.5 (1.76); jsmn and tinyexpr not
  slower than today.
- **G5 The allocator.** A typed allocation and its release at most 1.0
  times the system allocator's on the pairs benchmark (1.56 today), on
  darwin-arm64 and in the Linux container against glibc's.
- **G6 Nothing believed is wrong.** Built fast under
  `-fweavec-checks=verify`, no `weavec.proven`: both ports, both
  conversions' tests, RFC 0044's and RFC 0046's seeded bugs that compile.
  Built `full`: the seeded bugs that stop on main stop. Built fast: the
  list of seeded bugs that stopped on main and no longer do, each read;
  expected, only uses of a pooled block by code that is not safe.
- **G7 The loop is not worse.** The json port written again by a fresh
  agent: rounds and time beside RFC 0049's (2 rounds, 5 min 1 s). Safe
  C's five conversions under §5.1: every new error carries a fix, and
  `--weavec-fix` alone clears at least four in five of them.
- **G8 Nothing elsewhere.** A unit with no safe function compiles to
  main's object but for the ABI number. The `dev` tests, the cases, the
  quick corpus gate and the documentation's examples pass; the Linux
  container's `ci-release` passes.
- **H1 Hygiene.** The library within 31,850 lines (D6), the runtime
  within 11,000.

## Annotation surface

None. `resources/include/weavec.h` stays at version 3.4.

## Diagnostics

No rule identifier and no report kind is added.

**An error, in a new language** (§5.1), with RFC 0048's wording and RFC
0049's note, pinned in `test/Safe/typed-rules.c`:

```text
error: '<p>' points to one object, and '<function>' may access more than its <n> byte[s] through it [weavec-safe:call-args]
note: fix (likely): declare it '<declaration text>'; '<n>' is its count in the call to '<function>' at line <l>
```

**An error, in a new place** (§2.3), pinned in `test/Safe/cxx-views.cpp`:

```text
error: '<p>' points to one object, and '<view type>' is made over <count> elements of it [weavec-safe:call-args]
```

**A report, in a new place** (§2.3), the existing kind and line, pinned
in `test/Guards/view-made.cpp`:

```text
weavec: pointer-out-of-bounds at <file>:<line>:<col>: read of <n> bytes at <address>
```

**A report that no longer appears** in a fast program (D3):
`heap-use-after-free` for an access by code that is not safe to a pooled
block between its release and its reuse. `test/Guards/typed-reuse.cpp`
pins that it appears under `full`.

## Drawbacks

- **A fast program detects less in its code that is not safe** (D3). The
  interval it gives up is short and already closed for safe code, but it
  was a stop and becomes a read of zeroes.
- **More of the library is trusted.** Every trusted member is a line of
  libc++ or libstdc++ this project now relies on being checked, in each
  version it meets. S0 (c) reads them once; a new library version can
  change one, and only `verify` would say so.
- **The third attempt at this column.** Two gates were set and missed.
  (D5) is there so that this one fails in a day and not after it is
  built, but a gate that passes on four programs is still four programs.
- **Existing Safe C breaks** (D4), including this project's own
  conversions, for an error whose run-time form already existed.
- **Nothing here helps a conversion finish.** The unions, the `void *`
  callbacks, the objects that delete themselves and the cursors no bound
  names (RFC 0049 G5) are as they were, and the code left guarded for
  them still runs at the guarded cost inside a fast program.
- **The allocator gets a second path** to keep equal to the first in
  every corner (a signal handler inside the allocator, a thread's exit,
  a sweep meeting a slot whose shadow says live and whose word says
  released).
- **The line limit rises a tenth time** (D6).

## Alternatives

- **Fixes that rewrite** (RFC 0049's _Future work_). Argued in
  _Motivation_: it works on the column that is already level, with the
  tool that did not move it.
- **Trust the code that is not safe, in a fast build**, as Rust trusts
  `unsafe`: no guard in a `WEAVEC_GUARDED` function or an unconverted
  one, and the shadow gone from a fast program altogether. It is the
  largest lever left for conversions (145 of tinyxml2's 271 guards; RFC
  0048 measured that conversion with no check at all at 1.14 times
  Clang, the allocator's share). It gives up the one thing a conversion
  has that a Rust rewrite does not, which is that the part nobody made
  safe is still checked. It should be decided by itself, after this RFC
  shows what safe code costs when nothing is checked twice; it does
  nothing for new code, which has no such part.
- **Trust all of `std`.** A bad comparator makes `std::sort` read
  outside its range, and no hardening mode short of `debug` checks it.
  The structural rule stays, less two clauses.
- **The system allocator for fast programs.** On Darwin it already keeps
  types apart. It promises nothing (Y1) needs: a slot may be reused for
  another type under pressure, and there is no word to stop a second
  release.
- **Only the allocator, or only the views.** Each alone leaves the json
  port near 1.3. The table of §1 is one argument, and half of it would
  leave the next RFC to make it again.
- **Do nothing on cost and build a project tool.** A `cargo` for safe
  C++ would make the first five minutes easier and leave the answer to
  "is it as fast" where it is.
- **Checked conversions from `void *`**, using the type a pool already
  records, to make nng's 632 adapters unnecessary. The most promising
  thing RFC 0049's records point to for C, and a design of its own;
  _Future work_.

## Prior art

- **Rust.** Safe code pays a bounds comparison at an index and nothing
  for the shadow it does not have; `std` is trusted code behind checked
  interfaces. §1 is that bargain, stated for a compiler that also has a
  shadow.
- **libc++'s hardening modes** and Google's report on turning the `fast`
  mode on across its C++ (2024): about 0.3% of run time. The library's
  own check is cheap enough to be the only one, and Clang's build of the
  json port with `extensive` hardening confirms it here.
- **Apple's `-fbounds-safety`**: `__counted_by` pointers checked by the
  compiler, and library calls on them rejected or checked where they
  are written. (D4) is its behavior for C.
- **Apple's `kalloc_type` and xzone malloc, PartitionAlloc, GWP-ASan's
  successors**: allocators that keep types or size classes apart with a
  fast path of a pop and a push and no side table written per
  allocation. §3.2 is their path with RFC 0045's stricter promise.
- **mimalloc** (Leijen et al., 2019): free lists per page and thread so
  that the common allocation is a pop with no atomic operation.
- **AddressSanitizer's container annotations**: the shadow following a
  container's size at every change, which RFC 0045 already switched off
  in fast programs for the same reason (D3) gives.
- **Chromium's `base::span` and `-Wunsafe-buffer-usage`**: a code base
  moved to views because a view's check is the only one it needs.
- **RFC 0048 §6**, which is §2.1, §2.3 and §2.4 here, accepted and not
  built.

## Unresolved questions

1. **Is the time where the samples say?** S0 (a) and (b). Samples
   attribute by function; a guard also costs the code around it
   (registers, a loop the vectorizer leaves alone).
2. **Arrays in pools** (§3.1): how often a slot's size changes, and
   whether exact shadow for them is affordable.
3. **Does a sweep or a report rely on a pooled slot's shadow saying
   released?** The sweep reads pooled slots' bytes (Y1); the reports name
   a block's state from its word. `rt_test`'s `pool-*` and `sweep-*`
   tests are where an assumption would show.
4. **libstdc++.** Its `std::span` and `std::string_view` assert under
   `_GLIBCXX_ASSERTIONS`; are all the members of §2.1 covered, and do the
   `.weavec.t` clones inline as libc++'s do?
5. **A view by reference at a validating entry** is believed (Y-A1). Is
   that right for `const std::string_view &`, which code that is not safe
   can build from any pointer and count? Checking it costs one range
   guard per entry.
6. **Transparent hashing.** `find` with a view is trusted by §2.2. Does
   any library function with a view parameter keep the view past the
   call (a `std::string_view` stored by a library class the kinds do not
   call a view)?
7. **The LRU port** is at 1.24 with and without RFC 0048's change, and
   nobody has sampled it. G2 asks 1.10 of it without evidence that §2
   and §3 are what it lacks; S0 (a) and (b) time it and say.
8. **Safe C's frames** are still tracked in the fast mode (RFC 0048
   §10). G4's thresholds assume the allocator and §5 are most of Safe
   C's cost; S0 (e) should sample cJSON once more to see.
9. **Whether (D4) should wait for the fixer to run first** in this
   project's own conversions, so that the breaking change arrives with
   its edits.

## Future work

- **The decision about code that is not safe in a fast build**
  (_Alternatives_), once G2 and G3 say what is left.
- **Iterators held in variables and the algorithms** (§2.4): bounded
  iterators in libc++ (`_LIBCPP_ABI_BOUNDED_ITERATORS`) would give them
  the library's check to be trusted on.
- **Checked conversions from `void *`** by a pool's type, and tagged
  unions, for what conversions leave guarded.
- **Safe C's plain frames** (RFC 0048 §10).
- **A cost gate that stands**: the programs of G2 to G4 as a CI job that
  fails when a ratio rises, so that the column is not lost again.
- **Fixes that rewrite**, if conversions at libzmq's size show the
  agent's time going where a proposed rewrite would save it.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what S0 and the gates found. They override the body.

1. **S0 and (D5).** Measured on the `json` port of RFC 0049's G7 (median of
   seven, interleaved; `build/rfc50/s0/`): 1.55 times Clang on main; 1.48
   with the views trusted alone (a); 1.34 with the pooled slots' shadow
   kept alone (b, first half); 1.30 with both; 1.26 with §3.2's short
   paths; 1.22 once a type's pools were found without the lock (amendment
   7). The decision of (D5) was met at 1.22 and the work went on. (c)
   libc++ 22's `fast` mode checks every member §2.1 lists, so the table has
   no `unchecked(...)` clause; libstdc++'s were not read one by one, and the
   Linux container's tests, `ir-views.cpp` and `view-made.cpp` among them,
   pass with its assertions. (d) §5.1 newly rejects four lines of
   `test/Safe/` (two latent overflows in the tests themselves, which now
   carry a bound, and a sixth error in `fix-bounds.c`, with its fix), and
   nothing in `test/cases/`, the documentation's examples or RFC 0041 to
   RFC 0043's five conversions. (e) The reference test costs nothing
   measurable (cJSON 2.61 s with and without it, sds 1.13 s): §5.2 changes
   nothing. (f) Was not investigated.
2. **The table (§2.1).** A class row's trait `checked` names the views:
   `class std::basic_string_view : checked;`, `class std::span : checked;`.
   A checked view is trusted only where the compiler finds its pointer and
   its count (a member, or a member of a member, as libstdc++ keeps a
   `std::span`'s count in an extent's storage; or a static extent): without
   them §2.3 could not check the views it makes. A member that takes a
   pointer, an iterator or a callable stays guarded, structurally, as RFC
   0045's rule says; the constructors from a pointer and a count are among
   them, and §2.3 covers what they make.
3. **Where a view is made (§2.3), as built.** Every constructor of a
   checked view that is not trusted (it takes a pointer or an iterator)
   and that a safe function of a fast unit calls is followed by one range
   guard over the view it made, read from the view's own pointer and count
   (`guardMadeViews`, ledger reason `view`): not a comparison with the
   pointer's bound. One mechanism covers a pointer and a count, two
   pointers and two iterators alike. A view one too long therefore stops
   with the guard's report (`heap-buffer-overflow at …: read of 9 bytes`,
   pinned in `test/Guards/view-made.cpp`), not `pointer-out-of-bounds`.
   A pointer to one object is already rejected by RFC 0047's `call-args`
   ("argument 1 of 'basic_string_view' must have bounds"), pinned in
   `test/Safe/cxx-bounds.cpp` and `cxx-expressions.cpp`: the new wording of
   _Diagnostics_ was not added.
4. **A view from code that is not safe is believed, not guarded.** §2.3's
   range guard at a validating entry for a view parameter taken by value,
   and at the result of a project function that is not safe, was not
   built: a view's IR is coerced differently on each target, and the
   entry would need the layout of every view type a signature names. A
   view arriving from code that is not safe is believed as a reference is
   (Y-A1); the guarantees page extends Y-A4 to say code that is not safe
   passes, returns and stores views that point to their elements. This is
   a departure from the design, and unresolved question 5 now covers
   views by value too.
5. **The range-based `for` (§2.4), as built.** Clang puts the hidden
   iterator's calls (begin, end, the comparison, the step and the
   dereference) at the loop's colon. The checker records, for each loop
   of a safe function over an array, a library container or a checked
   view whose colon is not in a macro, `<file>:<line>:<column>:<symbol>`
   for each of those calls (the dereference alone of the loop variable's
   initializer), and `cloneTrusted` sends the call of a safe function
   made at that place to that callee to its trusted clone. For a pointer
   iterator (a `std::string_view`'s, an array's), the load at the colon is
   a `trusted` row. A loop variable that is a reference into a pointer
   iterator's range (`for (const char &c : view)`) keeps the guards of its
   uses. The range expression is not restricted: the rules follow
   `__range` as the borrow it is.
6. **Hardening (§2.5)** is read from the unit's preprocessor at its end:
   libc++ with `_LIBCPP_HARDENING_MODE` naming anything but `none`, or
   libstdc++ with `_GLIBCXX_ASSERTIONS`. Without it no library function is
   trusted, RFC 0045's owners included: the hole S0 (c) was to look for
   was there (a unit with `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE`
   trusted every owner's members), and is closed.
7. **The allocator (§3), as built.** (a) The shadow is kept only for a
   slot safe **C++** released (a new site flag, `WeavecRtSiteShadowed`,
   64). Safe C's reference test (O4) reads a released block's shadow; with
   (D3) applied to Safe C, `ownership/reference-dangling.c` stopped
   stopping, a weakening of a safe function's own check that (D3) did not
   cover. Safe C's pooled slots are marked released as before. (b) A slot
   whose next object has another size (an array's) takes that object's
   shadow on the short path: no class-wide shadow for arrays was built.
   (c) The short paths (`poolTake`, `poolGive`) are in the runtime, not
   inlined into programs; the claim stays a compare-and-swap, since Safe C
   has no thread rules. (d) A type's one-entry cache of its pool missed
   for every type allocated in several classes (strings' and vectors'
   buffers: 8.7 of the `json` port's 12.5 million allocations), and each
   miss took the pools' lock: pools are now found in their table without
   it (`lookPool`). (e) On Linux the thread's cache is read from an
   initial-exec thread-local copy of its key's value, not through
   `pthread_getspecific`. (f) §3.3's memory was not reduced: a slot keeps
   a byte past its object (RFC 0042 §8), so a 16-byte object takes a
   32-byte slot, and the pools keep up to the most the heap held live.
8. **Verify (§3.1, §4).** A unit compiled `-fweavec-checks=verify` calls
   `__weavec_rt_verify_program` from a constructor; the runtime then marks
   every pooled slot released. `WEAVEC_RT_ABI` is 46. The ledger stays at
   version 12; its rows gain the reason `view`, which a later reason does
   not replace.
9. **Safe C (§5.1), as built.** `libraryAccess` runs for C, and sees
   through a C library's fortified macro (Darwin's `memcpy` is
   `__builtin___memcpy_chk` inside a system macro) by the alias's own
   argument map; the message names the table's row (`memcpy`). The
   sentence of §5.1 about string functions was not built: neither language
   rejects a plain `char *` given to `strchr`, and (D4) was "as it has been
   in safe C++". A string such a call reads is checked when it runs, as
   before.
10. **Line limit (D6).** The library is 31,850 lines of 31,850: no headroom.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset) on
2026-11-18 and 2026-11-19, against main at `bffda036`, and in the Linux
container (aarch64, libstdc++, glibc). The evidence is in `build/rfc50/`
(not committed; `README.txt` there says what each file is). Times are
medians of seven interleaved runs against Clang's `-O2` build of the same
source.

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass.** `test/Guards/ir-views.cpp` (no shadow at a view's index, `front`, `substr`, a span's index, a range-based `for` over a vector, a view and an array; the guard kept for an iterator in a variable, with libc++'s hardening `none`, and under verify), `view-made.cpp` (the guard where a view is made: one too long stops there, one that fits runs), `typed-reuse.cpp` ((D3): guarded code reads the pooled block as zeroes in the fast build, stops in `full` and with a verify unit), `test/Safe/typed-rules.c` (§5.1); `cxx-bounds.cpp` and `cxx-expressions.cpp` already pin `call-args` for a view from a pointer to one object (amendment 3); `rt_test` `pool-keeps-shadow` (a second release included); `LibrarySpecTest` `ClassRowsNameCheckedViews`. `ctest --preset dev` passes 53 of 53; the Linux container's `ci-release` 53 of 53. |
| G2 New code | **Fail.** RFC 0049's G7 `json` port 1.22 (1.55 on main; asks 1.15), 158 MiB against Clang's 120 (1.32; asks 1.15); RFC 0044's `json` port 1.37 (1.72); the LRU port 1.25 (1.24; asks 1.10), of which a fixed 3 ms is the runtime's start (an empty program takes 5.2 ms against 2.2): run ten times longer it is 1.14, and Clang with libc++'s hardening and zeroed locals 1.02. Its hot functions keep no guard; what is left is not this RFC's. RFC 0044's Rust ports: 0.64 and 1.13. |
| G3 The conversions | **Fail.** tinyxml2 1.43 (1.44; asks 1.25), yaml-cpp 1.52 (1.58; asks 1.30). tinyxml2 spends 8% of its time in `__weavec_rt_range`: each node `new` places into the arena's `void *`, the result of a guarded function, which gets a range guard of 120 or 128 bytes where it arrives (RFC 0048 §7), and a range that long is a call. |
| G4 Safe C | **Fail.** User time against Clang's, main then this branch: cJSON 2.01 and 1.99 (asks 1.5), sds 1.63 and 1.61 (asks 1.3), inih 1.74 and 1.74 (asks 1.5), jsmn 1.28 and 1.27, tinyexpr 1.16 and 1.16 (not slower: pass). (D3) does not reach Safe C (amendment 7), cJSON allocates through its hook pointer, which is not typed, and the reference test costs nothing (amendment 1). |
| G5 The allocator | **Fail.** A typed allocation and its release, in pairs: Darwin 18.7 ns against the system allocator's 17.0 (1.10; 26.6 on main); Linux 18.2 ns against glibc's 10.5 (1.73; 22 before amendment 7 (e)), and 24.9 against 14.6 (1.70) in a noisier run. |
| G6 Nothing believed is wrong | **Pass.** Built fast under `-fweavec-checks=verify`, no `weavec.proven`: RFC 0044's 10 seeded bugs that compile as safe, both `json` ports' 646 vectors (645 pass, as with Clang), the LRU port's tests, tinyxml2's 517 checks. Built `full`, the 10 stop. Built fast, one seeded bug that stopped on main no longer does: `b094`, a pointer into a vector's old buffer after the vector grew, read by a guarded library function (`operator+` on a `const char *`) after the buffer went back to its pool: (D3), as designed. yaml-cpp's tests were not run under verify. |
| G7 The loop | **Pass.** The `json` port written again by a fresh agent with the frozen compiler: 2 rounds to a clean build, 5 min 17 s to `check.py`'s 645 of 646 (RFC 0049: 2 rounds, 5 min 1 s); one `owner-moved` on a move guarded by a flag, which it rewrote. Its port runs at 1.22 (Rust 0.88). Safe C's five conversions: §5.1 adds no error to any of them. |
| G8 Nothing elsewhere | **Pass.** 1,210 guarded compiles of `test/cases/` and `test/Guards/` give main's object (`../weavec-rfc47-main`, the same passes and runtime as main): 536 identical, 674 differing only in the immediate that carries the runtime's ABI, 45 then 46. The `dev` tests, the cases, the documentation's examples and the site's tests pass; the Linux container's `ci-release` passes. |
| H1 Hygiene | **Pass.** Library 31,850 of 31,850 lines (D6); runtime 10,837 of 11,000. |

What the numbers say about the owner's goal: new code an agent writes in
safe C++ costs a fifth more than Clang's build of it, where it cost half
more; the agent's loop did not slow down; nothing believed was wrong. That
is the largest single step any of the cost gates has seen, and it is not
Rust's cost: (D1) holds this RFC at Accepted. What is left is measured
and named. For new code: the runtime's start (3 ms), memory (a byte past
each object rounds 16-byte objects to 32-byte slots), the allocator's
path, which is a call through two layers and a compare-and-swap where
glibc's is a few loads, and library code that takes an iterator. For
conversions: the guarded code around safe code, whose results get range
guards, and which is the decision _Alternatives_ deferred. For Safe C:
untyped allocations and tracked frames.
