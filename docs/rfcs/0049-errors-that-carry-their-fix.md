# RFC 0049: Errors that carry their fix

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-16
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Carries out what
  [RFC 0047](0047-conversion-in-place.md) and
  [RFC 0048](0048-safe-code-without-the-shadow.md) both deferred (_Future
  work_: "Fix-its for the notes that name an annotation, and a mode that
  applies them") and answers gate G4 of RFC 0047, which failed. Supersedes
  RFC 0047's §7 for how the survey is recorded (the per-function counts
  become a list of errors) and for the plan the audit prints. Corrects
  one case RFC 0047's amendment 1 left open: a constructor's member
  initializer is not checked as an assignment is (§4). Adds no
  annotation, no rule and no report kind. Changes the wording of the
  notes of the rules that gain a fix, the enforcement ledger (version 12)
  and the audit (version 8). The runtime is not touched.

This RFC was written at the request of the project owner on 2026-11-16,
who asked a ninth time for the single change that would do most to make
an AI rewrite into C or C++ as attractive as an AI rewrite into Rust, to
be implemented next, with breaking changes and the removal of old code
welcome. The measurements in _Motivation_ were taken on main at
`392b7201` (RFC 0048 merged), darwin-arm64, the `dev` build, Homebrew
LLVM 23.1.0, and from the evidence RFC 0047 and RFC 0048 left in
`build/rfc47/` and `build/rfc48/`. The probe and the scripts are in
`build/rfc49/` (not committed; `README.txt` there says what each file
is). Nothing of the design has been built or prototyped; stage S0 is
where it meets code.

The owner accepted the design as drafted on 2026-11-17, before any of it
was built, and with it the six decisions that were theirs to make. They
are marked **(D1)** to **(D6)** where they are argued, and collected
here. The thresholds of §11 and the _Unresolved questions_ are still
open, and S0 is where they are settled; what it measures may reopen (D4)
and (D5).

- **(D1) The compiler writes its part of a conversion.** Every rule error
  whose remedy is an edit the compiler can write carries that edit, as
  data: where, what text, and how sure. Three levels: _certain_,
  _likely_, _choice_.
- **(D2) A fix is source text and nothing else.** It is proposed from
  what one function shows, merged for each declaration by the driver, and
  checked by the same rules as an annotation a person writes. No pass
  uses a fact that is not written in a declaration, and no analysis
  crosses a function. This is the line RFCs 0001 to 0034 were on the
  wrong side of.
- **(D3) `weavec-cc --weavec-fix` applies the fixes over a project and
  repeats by itself.** A survey's ledger records each unit's command
  line; the fixer edits the sources, runs the rules again on the units an
  edit reached, and stops when a round applies nothing. One command is
  the mechanical part of a conversion.
- **(D4) Certain and likely fixes are applied; choices are printed.** A
  likely fix that brings an error with no fix is withdrawn. A file left
  with no error is marked `WEAVEC_SAFE_FILE`.
- **(D5) A pointer parameter stored in a member of a class of plain
  values makes the member `WEAVEC_BORROWED`, not the parameter
  `WEAVEC_ESCAPES`.** A slice or a cursor is a view. Everywhere else the
  fix of `escape` is `WEAVEC_ESCAPES`. This is the decision the author is
  least sure of.
- **(D6) The gate is the one RFC 0047 failed, and S0 decides before the
  rest is built.** A conversion in at most half the agent time of a
  rewrite into Rust, on the two libraries measured before and on one
  project no RFC has read. The library's line limit rises from 27,600 to
  29,300; the runtime's stays.

## Summary

Eight RFCs gave C and C++ the rules, and by their own gates an agent can
iterate on them. What the gates do not show is a reason to choose them.
Converting tinyxml2 in place took an agent 1,201 seconds; rewriting it in
Rust took another agent 1,145. The conversion runs at 1.44 times Clang
and the Rust port at 0.8 to 1.0. A team that asks which to do is told:
the same work, a slower result.

The one thing C and C++ have that a rewrite cannot have is the code
itself. The declarations are there, the uses are there, and most of what
a conversion adds says what the code already does: this pointer has
`size_` elements, this parameter is kept, this cursor is a string. The
compiler can see that. Today it prints a sentence about it and waits for
an agent to work out the edit, apply it, compile, and meet the next
layer. Of the errors the survey finds on four unedited projects, 60% are
of this kind. The implementer of RFC 0048 met its own conversion gate
with a script that parsed the compiler's messages and edited the files,
in a loop.

This RFC moves that loop into the compiler:

- **Errors as data.** An error carries its fix: the edits, a level of
  confidence, and the evidence. The note under the error says the exact
  text to write and where, and carries it as a Clang fix-it.
- **Declarations from evidence.** For an error that asks for a fact
  about a declaration (a bound, a terminator, an end, what is kept, what
  a result borrows from), the compiler proposes the fact from the uses in
  the function it is compiling, with the library table's pointer-and-count
  pairs as the strongest witness.
- **`weavec-cc --weavec-fix`.** It merges the proposals of every unit
  for each declaration, writes the ones that agree, runs the rules again,
  and repeats until nothing changes. What is left is what needs
  judgment, grouped by the choice that would clear the most.

Nothing changes in what a compiled program checks. A fix is an annotation
or a small edit in the source; the rules and the checks treat it as they
treat one a person wrote.

Expected result (gates, §11): at least half of the distinct errors on the
unedited corpus cleared with no agent, and four in five of those that ask
for a declaration; tinyxml2 converted again by a fresh agent in at most
570 seconds, half of what the Rust rewrite took, and yaml-cpp in 580,
half of what it took before; one project of at least 50,000 lines that no
RFC has read, converted inside a fixed budget and recorded.

## Motivation

### Where the comparison stands

| | Agent time | Rounds | Run time, times Clang |
| --- | --- | --- | --- |
| tinyxml2 rewritten in Rust (RFC 0047 G4) | 1,145 s | 4 | 0.8 to 1.0 |
| tinyxml2 converted in place (RFC 0047 G3, RFC 0048 G5) | 1,201 s | 6 (clean at 3) | 1.44 |
| yaml-cpp converted in place | 1,161 s | 18 | 1.58 |
| `json` port, Rust | 7 min 29 s | 1 | 0.94 |
| `json` port, safe C++ | 9 min 7 s | 3 | 1.72 |

RFC 0047 set its gate G4 at half the rewrite's time and measured 1.05
times. Its closing paragraph said what that meant: "For a 5,400-line
library the Rust agent finished as fast, and its result runs faster; the
conversion's case rests on what the gate did not measure." RFC 0048 then
worked on the last column and moved it from 1.72 to 1.44, short of its
1.25. Nothing since RFC 0047 has worked on the first column.

The two columns are not the same kind of problem. What is left of the
last one is the allocator's path, range guards in loops and the guarded
code around safe code (RFC 0048, _Measured gates_): engineering that has
been named twice and needs no decision. The first column has never been
attacked, and it is the only one in which C and C++ can be ahead of Rust
and not merely level with it. A rewrite writes every line again. A
conversion should cost in proportion to what the code does not already
say.

### Measured: what the errors ask for

`build/rfc49/survey/shapes.py` counts the distinct errors of RFC 0048's
survey (G10: leveldb, pugixml, tinyxml2 and snappy, unedited, about
27,000 lines of sources): 1,957, seven for every hundred lines.

| Rule | Errors | Share | What clears one |
| --- | ---: | ---: | --- |
| `index-single` | 785 | 40% | a bound on the pointer's declaration |
| `escape` | 303 | 15% | `WEAVEC_ESCAPES` on a parameter, or `WEAVEC_BORROWED` on a member |
| `owner-path` | 258 | 13% | `WEAVEC_ALIASED` on a class, mostly |
| `raw-owner` | 246 | 13% | an owner (`std::unique_ptr`, a container): a rewrite |
| `pointer-cast` | 163 | 8% | a rewrite, or a guarded helper |
| `call-args` | 106 | 5% | a bound on a declaration |
| the other eight | 96 | 5% | |

Three message shapes are two thirds of everything: "points to one object
and cannot be indexed or offset" (785), "is reached through a pointer
kept in memory" (258) and "the address of … is kept by …, and … only
borrows it" (253). The 785 `index-single` errors name 138 distinct
pointers, the 303 `escape` errors 126 names, the 106 `call-args` errors
61. An agent is shown 1,194 errors whose answer is about 325
declarations.

### Measured: a class of 25 lines takes four rounds

`build/rfc49/probe/slice0.cpp` is the guide's `Slice` (a `const char
*data_` and a `std::size_t size_`, a constructor from a pointer and a
count, `operator[]`, `remove_prefix`, `starts_with` with `memcmp`,
`ToString`) with no annotation, and a helper that counts a character in
`(const char *text, std::size_t n)`.

- **Round 1**: 7 errors. Two `index-single` and three `call-args` on
  `data_`, one `escape` on the constructor's parameter, one
  `index-single` on `text`. The note for each bound is the same sentence:
  "give its declaration a bound: 'const T \*WEAVEC_COUNTED_BY(n) p' (or
  WEAVEC_ENDED_BY, WEAVEC_NULL_TERMINATED), take a 'std::span', or index
  an array or a container". Which bound, counted by what, is left to the
  reader. The file answers it four times: `memcmp(data_, x.data_,
  x.size_)`, `std::string(data_, size_)`, `size_ -= n` beside `data_ +=
  n`, `i < n` above `text[i]`.
- **Round 2**, the three declarations annotated as the notes say: 1
  error. `data()` returns the member as a plain pointer and a caller
  passes it for a counted parameter.
- **Round 3**: clean. The program then stops when it runs:
  `bounds-conversion at slice1c.cpp:9:72: needs 5 bytes`, in the
  constructor, whose parameter has no bound and whose member now has one.
  (A caller that passes `text.data() + 2` gets a compile error for the
  same missing bound, as a position.)
- **Round 4**, the parameter counted: clean, and the program stops
  again, with `stack-escape`, where a `Slice` is made over part of a
  local `std::string` of seven characters. The constructor was declared
  `WEAVEC_ESCAPES`, as round 1's note said and as the guide's own example
  is, and a short string's characters are in the string object, on the
  stack. With a string of 51 characters the same program runs. With the
  member declared `WEAVEC_BORROWED` and no `WEAVEC_ESCAPES`
  (`slice3b.cpp`), it compiles and runs with either.

Every one of the edits is a token on an existing declaration, and every
one but the last follows from the file. The last is a decision the note
made for the reader, and made wrong for a view (D5). A rules-only compile
of all of tinyxml2 takes 0.19 seconds on this machine and reports 244
errors. An agent's round on yaml-cpp took 64 seconds on average (1,161
seconds, 18 rounds), and tinyxml2's 200.

### Measured: what the conversions were made of

`build/rfc49/share.py` compares a conversion with its original and
counts the added lines that differ only by `WEAVEC_` annotations.

| | Lines added | Only annotations | File markers and blank lines | Other |
| --- | ---: | ---: | ---: | ---: |
| tinyxml2, RFC 0047's conversion | 333 | 91 | 8 | 234 |
| tinyxml2, after RFC 0048's rules | 483 | 241 | 8 | 234 |
| yaml-cpp, RFC 0047's conversion | 387 | 50 | 222 | 115 |
| yaml-cpp, after RFC 0048's rules | 408 | 71 | 222 | 115 |

Half of what was added to tinyxml2 and seven tenths of what was added to
yaml-cpp is a marker or an annotation on a line that is otherwise the
original's. The step RFC 0048 asked of each was annotations and nothing
else: for tinyxml2 143 lines (132 `WEAVEC_NULL_TERMINATED_OR_NULL`, which
its gate records as written "by one pattern", and 16 `WEAVEC_ESCAPES`),
for yaml-cpp 20 `WEAVEC_ESCAPES` "up four call chains" and one
`WEAVEC_BORROWED`. They were not written by an agent reading errors.
`build/rfc48/g4/tools/escapes.py` reads the compiler's errors on standard
input, finds "the address of 'X' is kept by …, and 'F' only borrows it"
with a regular expression, and inserts the annotation in every
declaration of `F`; `escape-rounds.sh` runs it for up to eight rounds.
That is this RFC, written in thirty lines against the text of the
messages, because the compiler had no other interface.

The agents said the same in their records. tinyxml2's: its time includes
"small experiments to learn what the annotations accept", and
"`WEAVEC_LIFETIMEBOUND` fixes this, but neither guide mentions it for
this case; I found it by experiment". yaml-cpp's: seven `call-args`
errors "caused by my own bound annotations" at the callers, a round
later, and four `index-single` left "because `WEAVEC_ENDED_BY` on a
`const char *&` parameter had no effect".

### What an agent has in Rust that it does not have here

- **A suggestion with the error.** `rustc` prints the edit (`help:
  consider borrowing here: '&x'`), marks whether a machine may apply it,
  and `cargo fix` applies those. No error of WeaveC's carries an edit:
  `grep -r FixItHint lib include tools` finds nothing.
- **A prior.** A model has read a great deal of Rust and none of
  WeaveC's dialect. Everything it knows about `WEAVEC_COUNTED_BY` it
  learns from a note, a rule's page, and trial. The note is therefore the
  product, and a note that says the exact text is worth more here than in
  Rust.
- **A migration tool.** Rust moves its whole ecosystem between editions
  with `cargo fix --edition`. A conversion to safe C++ is an edition
  migration with no tool.

### Why this, and why now

The rules stopped moving. RFC 0047 gave raw pointers their bounds, RFC
0048 settled what a plain pointer is and what is kept. Before that a
fixer would have chased a design; now the notes that name one annotation
are stable, and the survey (RFC 0047 §7) already runs the rules over
every function of a project without failing its build. What is missing is
that an error is a string.

## Soundness

### Nothing new is believed

No guard is added or removed by this RFC, no rule removes a guard on the
strength of a fix, and the runtime does not change. A fix is text in a
source file. After it is written, the compiler that reads the file knows
no more than if a person had typed it: the rules check the function that
holds the annotation, the conversions into a bounded pointer are compared
when the program runs, and the entries reached from code that is not safe
read the shadow, as RFC 0042, RFC 0047 and RFC 0048 say.

What does change is who writes the declarations that typed memory rests
on, and how many are written at once. In plain terms:

- **A `WEAVEC_ESCAPES`, a `const` or a `WEAVEC_COUNTED_BY(0)` that
  should not be there** cannot make a program less safe. The first
  places a local that did not need it, and stops a caller that passes a
  pointer into a frame the rules did not place (`stack-escape`; round 4
  of the probe). The other two are rejected by Clang or by
  `index-single` where the declaration is used otherwise.
- **A bound that is too small** stops correct code with
  `pointer-out-of-bounds` when it runs. A test finds it, and the report
  names the access; the fixer's log names the declaration and the
  evidence it was written from.
- **A bound that is too large** is compared wherever safe code makes the
  pointer (`bounds-conversion`, `counted-pair`) and at every validating
  entry, so safe code cannot be handed fewer elements than it was
  promised by safe code or across an entry. It is **not** checked where
  code that is not safe stores into a member or a global that safe code
  loads: that is assumption T-A1 of RFC 0042 and V-A1 of RFC 0048, which
  say such code keeps typed memory. A fixer that writes three hundred
  bounds leans on that assumption three hundred times, where an agent
  that wrote them one at a time read each. The checked build keeps every
  guard and `verify` monitors every believed access, and §8 lists in the
  audit each likely bound on a declaration that code which is not safe
  writes, so that the ones that rest on the assumption can be read.
- **A wrong `WEAVEC_BORROWED`** (D5) makes a class a view. RFC 0044's
  rules for views then apply to it, which reject more, not less; and RFC
  0048's `Kept` no longer counts a store into that member as keeping.
  What stands behind the member afterwards is (X3) and (X4), as for a
  view a person declared.
- **A wrong `WEAVEC_LIFETIMEBOUND`** would let a caller believe a result
  does not point into the receiver. It is proposed only where every
  `return` of the function's body returns the parameter, a position in
  it, or null, which is the fact itself at the time it is written. If the
  body changes later the annotation is as stale as a hand-written one.
  S0 (c) establishes whether the rules reject that in a safe function
  today; if they do not, that is a hole of RFC 0044's and is fixed there.

### Bugs caught

One case moves from run time to compile time (§4):

```cpp
class Slice {
  const char *WEAVEC_COUNTED_BY(size_) data_;
  std::size_t size_;
public:
  Slice(const char *d, std::size_t n) : data_(d), size_(n) {}   // call-args
  void set(const char *d, std::size_t n) { data_ = d; size_ = n; }  // call-args today
};
```

The assignment in `set` is rejected today; the same pointer in the
constructor's initializer compiles, and stops with `bounds-conversion`
when the count is more than one.

A conversion may also find what a fix's evidence contradicts: two uses
that disagree about a pointer's count are listed as a choice with both
sites (§3.2), and one of them is often the bug. RFC 0047's G6 found
yaml-cpp's out-of-bounds read that way, by hand.

### Bugs deliberately not caught

Nothing is added to what the rules see. A fix proposed from wrong code
states what the wrong code does: a loop that runs to `n + 1` proposes no
bound (§3.2 admits only a name or a constant), and one that indexes with
the wrong count proposes the wrong count, as likely. The checks then hold
the program to the declaration, not to what was meant.

### Accepted rejections and false traps

- **§4** rejects a constructor that initializes a bounded member from a
  pointer to one object with a count that is always zero or one when the
  program runs. An assignment that does so is rejected already; the fix
  is the constant.
- **A likely bound that is too small** (above) is a false trap until the
  declaration is corrected.
- **A member made `WEAVEC_BORROWED`** makes every object of its class a
  view, and code that stores one past what it points into is rejected
  (`escape`). §6 withdraws the fix when that happens in the units the
  fixer can see.
- **A parameter made `WEAVEC_ESCAPES`** stops a caller that passes a
  pointer into a local the rules did not place, when it runs
  (`stack-escape`), as RFC 0048 says and as round 4 of the probe shows
  for a local string's own bytes. That is the rule's, not the fixer's,
  but the fixer writes the annotation without being asked twice.

### Assumptions

None is added. T-A1 and V-A1 carry more weight, as said above.

## Detailed design

### 1. An error is data

Every rule error goes through one function (`report` in
`lib/Frontend/SafeCheck.cpp`), with a rule, a message, a place and a
note. It gains a list of _fixes_ and a list of _subjects_.

A **fix** has:

- a **level**: `certain`, `likely` or `choice`;
- one or more **edits**, each a file, a byte offset, a length and the
  text that replaces it (an insertion has length zero), all of which are
  applied or none;
- the **declaration** it is about, named by the file, line and column of
  its first declaration's name (for an instantiation, the template's);
- for a likely fix, the **evidence**: its kind (§3.2) and its place.

The levels:

- **certain**: the edit says what the function with the error already
  does (it keeps the parameter, it returns the end, the variable is
  never assigned), or copies a declaration's annotation to its other
  declarations. It states a fact, so it is never withdrawn. What follows
  from the fact is the rule's doing: a caller's local may be placed, and
  a caller that hands a kept parameter a pointer into a frame stops.
- **likely**: the edit states a fact about a declaration that every use
  the compiler examined agrees with and that it cannot prove: a bound, a
  terminator, what a result borrows from, that a member borrows.
- **choice**: more than one edit would clear the error and they mean
  different things, or one edit would and it changes the design
  (`WEAVEC_ALIASED`). A choice is printed and counted, never applied.

An error with no fix keeps the note it has today.

The **subjects** of an error are the declarations whose stated facts the
rule read to reach it: the pointer and its bound, the parameter, the
member, the class and its kind. The fixer uses them to group what is left
and to withdraw (§6).

A fix is never proposed for a declaration in a system header, in a file
the unit cannot name by a path, or written inside a macro's expansion.
An edit inserts text and nothing else, except (C4) below, which inserts
`const`, and the file marker.

### 2. What a compile prints

The note under an error that has a fix is placed at the declaration,
says the text to write, and carries the edit as a Clang fix-it, so that
`-fdiagnostics-parseable-fixits` and an editor show it:

```text
slice.h:13:49: error: 'data_' points to one object and cannot be indexed or offset [weavec-safe:index-single]
slice.h:24:15: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(size_) data_'; 'size_' is its count in the call to 'memcmp' at line 19
slice.h:8:20: error: the address of 'data' is kept by 'data_', and 'Slice' only borrows it [weavec-safe:escape]
slice.h:24:15: note: fix (likely): declare the member 'const char *WEAVEC_BORROWED data_', a view's; or (certain) the parameter 'const char *WEAVEC_ESCAPES data' if 'Slice' objects are kept
registry.h:9:41: error: the address of 'c' is kept by 'push_back', and 'add' only borrows it [weavec-safe:escape]
registry.h:9:22: note: fix (certain): declare it 'Counter &WEAVEC_ESCAPES c', here and at registry.cpp:14:29; a caller's local then lives in typed memory
node.h:88:5: error: 'children_' is reached through a pointer kept in memory, and 'append' may change what it owns [weavec-safe:owner-path]
node.h:40:7: note: choice: declare 'Node' WEAVEC_ALIASED if its objects are changed through pointers other objects keep; or keep this function WEAVEC_GUARDED
```

A fix-it is attached to a note, never to an error, which is Clang's
convention for an edit that is not the only reading; Clang's own
`-fixit` therefore applies none of them, and `--weavec-fix` is the one
way they are applied.

A compile that printed at least one fix ends with one more note:

```text
note: 6 of 7 errors carry a fix; 'weavec-c++ --weavec-fix -- <this command>' applies them
```

In a plain compile a fix is proposed from the function that has the
error and, for a member, from the member functions of its class that the
unit defines. `--weavec-fix` adds what other functions and other units
show (§5).

### 3. The fixes, by rule

S0 (a) and (d) settle the list; what follows is what the survey and the
two conversions ask for.

#### 3.1 Certain

- **(C1) `escape`, a kept parameter**: `WEAVEC_ESCAPES` after the `*` or
  the `&` of the parameter, or after the parameter list for `this`, in
  every declaration of the function the unit sees and in the functions
  it overrides and is overridden by there. A caller that passes its own
  parameter on gets the same error and the same fix in the next round,
  which is how RFC 0048 says the annotation spreads, and the chain ends
  at a local, which is placed. (D5) names the one case that is not this.
- **(C2) `bad-annotation`, declarations that disagree**: the annotation
  one declaration has is written on the others.
- **(C3) `call-args`, a position that is the end**: where the value
  returned or stored is, by its form, the end of its own bound (`data_ +
  size_` of a pointer counted by `size_`, `&a[N]` of an array of `N`,
  the pointer a `WEAVEC_ENDED_BY` names), the result or the member is
  declared `WEAVEC_COUNTED_BY(0)`.
- **(C4) `thread-share`, a variable that never changes**: for a variable
  with internal linkage or a static local, initialized by a constant,
  that the unit never assigns and never binds to a reference or a
  pointer that is not `const`: `const` after its `*`, or before its type
  when it is not a pointer. Ten of tinyxml2's twelve were these.
- **(C5) The file marker.** Not the fix of an error: a file in which
  every function is safe or has no error, in every unit recorded, gets
  `#include <weavec.h>` and `WEAVEC_SAFE_FILE` after its last top-level
  `#include`. Where an annotation is inserted in a file in which the
  macros are not defined at that place, the include is added with it.

#### 3.2 Likely: a declaration's fact, from evidence

An error **wants** a fact about a declaration: `index-single` and
`call-args` want a bound on the pointer they name (for a local, on the
parameter, member or function result its value came from); a `call-alias`,
`stale-borrow` or `escape` error that holds a call's result to point into
its receiver or into each argument wants to know what the result borrows
from.

A function gives **evidence** about a wanted declaration it uses. Each
kind is decided inside that one function:

- **(E1) A library call says the count.** The pointer is an argument of
  a call whose row in the library table pairs it with a count
  (`memcmp(a, b, n)`, `memcpy`, `fwrite`, `std::string(p, n)`, the class
  rows' `counted` clauses): `WEAVEC_COUNTED_BY(n)`, or
  `WEAVEC_SIZED_BY(n)` where the row counts bytes and the pointee is not
  a byte.
- **(E2) A comparison bounds the index.** An index or a step of the
  pointer is made only under `i < n` or `i != n` (a loop's condition, an
  enclosing `if`, an `assert`): `WEAVEC_COUNTED_BY(n)`.
- **(E3) A comparison with a pointer.** The pointer is read or stepped
  only under `p < e` or `p != e`: `WEAVEC_ENDED_BY(e)`.
- **(E4) A terminator is tested.** The pointee is a character type and
  the function tests a byte it reads through the pointer against zero,
  passes the pointer where a row reads a string, or sets it from a
  literal or a terminated pointer: `WEAVEC_NULL_TERMINATED`, and
  `WEAVEC_NULL_TERMINATED_OR_NULL` when the function also compares the
  pointer with null or gives it null.
- **(E5) Constant indices only.** `p[0]` to `p[k]` and nothing else:
  `WEAVEC_COUNTED_BY(k + 1)`.
- **(E6) A bound flows in or out.** The declaration is set from, stored
  into, passed for or returned from a pointer whose bound is already
  declared, with the count that goes with it: a constructor's `d` stored
  into `data_`, counted by `size_`, beside `size_(n)`, is
  `WEAVEC_COUNTED_BY(n)`; `data()` returning `data_` is
  `WEAVEC_COUNTED_BY(size_)`.
- **(E7) What is returned.** Every `return` of the body returns one
  parameter, a position in it, or null: `WEAVEC_LIFETIMEBOUND` on that
  parameter.

A count is _admissible_ only when it can be written where the
declaration is, by the rules `bad-annotation` already enforces: for a
parameter, another parameter of its function or a constant; for a
member, another member of its class (`x.size_` beside `x.data_` is
`size_`); for a result, a parameter, or a member for a member function.
It must be a name or a constant after casts are removed: `n + 1` is not
evidence.

The proposals for one declaration are **merged**. (E4) with and without
null is the form with null. A named count and (E5) is the named count.
(E1) to (E3) and (E6) must agree on the bound exactly. If what remains is
one annotation, it is the declaration's likely fix, written on every
declaration the unit sees. If two remain, the fix is a choice that lists
both with their places, and nothing is written.

A function's own definition is the only place its parameters' and its
result's evidence is looked for; a member's is every function that names
it.

#### 3.3 Likely: a member that borrows (D5)

Where `escape` is reported because a parameter's pointer is stored in a
pointer member, and the member's class has no user-provided destructor,
copy or move, no base, no virtual function, and members that are all
scalars, pointers and views, the first fix is `WEAVEC_BORROWED` on the
member (likely) and the second is (C1) on the parameter (certain). The
note prints both, as in §2, and the fixer applies the first.

The reason is cost and reach. `Slice(buf, n)` with `WEAVEC_ESCAPES`
places `buf` in typed memory, one allocation each time its scope is
entered, and asks the same annotation of every function that wraps its
own parameter in a `Slice`: RFC 0048's survey shows `escape` on leveldb
rising from 12 to 274 with that rule, and the guide it wrote says "a
pointer member of a short-lived cursor or wrapper that only borrows is
`WEAVEC_BORROWED` instead, which keeps nothing". It also stops a correct
program: round 4 of the probe is a `Slice` over a local string's own
bytes, which `WEAVEC_ESCAPES` turns into `stack-escape` when the string
is short and the view rules accept. The other way round, a class with a
destructor or a container is not a view by any reading, and (C1) stands
alone.

#### 3.4 Choices

Printed with the error, counted in the plan (§8), never written:

- `WEAVEC_ALIASED` on the class an `owner-path` error names. Both
  conversions cleared most of that rule with three and five classes.
- The two bounds of a declaration whose evidence disagrees.
- (D5)'s second fix.

`WEAVEC_GUARDED` on the function is always possible and is not listed as
a fix: it is how a conversion gives up on a function. `raw-owner`,
`pointer-cast`, `union-pointer`, `string-write` and the rest have no fix
in this RFC (_Future work_).

#### 3.5 Safe C

The fixes above are in the rules Safe C and Safe C++ share
(`index-single`, `call-args`, C's `escape`, `bad-annotation`);
`thread-share` and (D5) are C++'s. Ownership's errors whose note names one
declaration and one annotation (a result returned from an owner on every
path, a parameter released on every path, `WEAVEC_DROPS`) become likely
fixes; S0 (d) lists them from the notes in `SafeOwnership.cpp` and drops
any whose evidence is not in the one function.

### 4. A constructor's initializer is checked as an assignment is

`call-args` rejects a pointer to one object passed for a parameter with
a bound, returned as a result with one, or assigned to a member with one
("'d' points to one object, and field 'data_' has a bound"). The probe
found the case it misses: the same pointer in a constructor's member
initializer, `data_(d), size_(n)`, compiles and is compared when the
program runs. RFC 0047's amendment 1 says the bounds' rules run over "the
function's body, its constructor's list and its lambdas", so this is a
correction and not a new rule; it is recorded here because it rejects
code that compiles today, and because without it the third round of the
probe is a failed test where it should be an error with a fix:

```text
error: 'd' points to one object, and field 'data_' has a bound [weavec-safe:call-args]
note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(n) d'; 'size_' is set from 'n' beside it
```

### 5. Evidence from other units

A plain compile sees one unit. The fixer's rounds (§6) pass each compile
a file of the declarations other units want (`-weavec-wants=<file>`, a
`cc1` option the driver does not document: each line a declaration and
what is wanted of it). A unit that defines a function whose parameter or
result is wanted, or a function that names a wanted member, records its
evidence for it in its ledger whether or not it has an error of its own.
Nothing else crosses a unit: no summary of a function, no fact about a
callee. A fact gets from one function to another by being written on a
declaration and read in the next round, which is how the rules themselves
compose.

### 6. `--weavec-fix` (D3, D4)

```text
weavec-cc --weavec-fix [--dry-run] [--level=certain|likely] [--no-mark]
          [--only=<path>,...] [--rounds=<n>] [-j<n>] <ledger>...
weavec-cc --weavec-fix [options] -- <a compile command>
```

Not a compile, as `--weavec-audit` is not. With ledgers it works on the
units they record; with a command after `--` it first runs that command
with the rules only, a survey and a ledger in a temporary directory.

A ledger written under `-fweavec-safe-survey` records, for its unit, the
directory and the command line it was compiled with, and each file of the
project the unit read with its size and a hash (§7). One round:

1. **Read** every unit's errors, fixes and evidence.
2. **Merge** by declaration (§3.2). Drop a fix whose file is outside
   `--only` (default: every file that is not a system header), is not
   writable, or has changed since its ledger was written; the last is an
   error that names the file and stops before anything is written.
3. **Apply** the certain fixes and, unless `--level=certain`, the likely
   ones: every edit of a file sorted by offset, identical edits from
   several units once, a fix whose edits overlap another's left for the
   next round.
4. **Run the rules again** for every unit that read a changed file: its
   recorded command with `-fsyntax-only`, the survey, a ledger in the
   fixer's own directory, the wants of §5, and without its output and
   dependency-file arguments; `-j` of them at a time.
5. **Withdraw.** If a unit now has an error with no fix that it did not
   have before the round, and a subject of that error is a declaration a
   likely fix of this or an earlier round changed, that fix's edits are
   taken out again, the declaration is not proposed for again, and it is
   listed among the choices with what each outcome left. Certain fixes
   are never withdrawn.
6. **Stop** when a round applied and withdrew nothing, or after
   `--rounds` (default 16). Then mark the ready files (C5), unless
   `--no-mark`, and run the rules once more for the units that read
   them.

It terminates: an annotation is only ever added to a declaration that
lacked it, a withdrawn declaration is not tried again, and there are
finitely many declarations.

`--dry-run` writes nothing and prints the edits of the first round as a
unified diff. Every run writes `weavec-fix.json` beside the first
ledger: each edit, its level, its evidence, its round, and what was
withdrawn and why. It prints:

```text
weavec: fix: round 1: 212 edits in 9 files (171 certain, 41 likely); 807 errors before, 388 after
weavec: fix: round 2: 44 edits in 6 files (39 certain, 5 likely), 2 withdrawn; 351 after
weavec: fix: round 3: nothing to apply
weavec: fix: marked 14 files WEAVEC_SAFE_FILE
weavec: fix: 256 edits in 11 files; 807 errors before, 351 after: 122 have a choice, 229 have no fix
weavec: fix: 'weavec-c++ --weavec-audit <ledger>' prints what is left; weavec-fix.json is the log
```

The fixer does not reformat, does not touch a line it does not edit, and
keeps each file's line endings. It does not know about version control:
`--dry-run` and the log are how a change is reviewed before and after.

### 7. The ledger (version 12)

- **Added, under the survey:** the unit's `directory`, `command` and
  `files` (each a path, a size and a hash).
- **Added:** `errors`, every rule error of the unit, printed or
  surveyed: `rule`, `file`, `line`, `column`, `message`, `function`,
  `subjects` and `fixes` (each a `level`, a `declaration`, `edits`, and
  for a likely fix `evidence`). And `evidence`, what the unit says about
  declarations other units want (§5).
- **Removed:** a function's `survey` (its errors by rule) and the unit's
  `surveyGlobals`. Both are counts of `errors`.
- **Changed:** a ledger is written when the rules have run, also for
  `-fsyntax-only` and for a unit that fails to compile; it then has
  `"partial": true` and the checker's part only, and the audit takes no
  access counts from it.

### 8. The audit (version 8)

The plan (RFC 0047 §7) is rewritten around what is left to decide:

```text
conversion: 412 of 1930 functions safe (21%); 388 more have no error
fixes: 171 certain, 46 likely ('weavec-c++ --weavec-fix ledger/' applies them)
choices, most errors first:
  WEAVEC_ALIASED on 'Node' (node.h:40)  46 errors in 31 functions
  'buf_': WEAVEC_COUNTED_BY(cap_) at buffer.cc:51, WEAVEC_COUNTED_BY(len_) at buffer.cc:88  9 errors
  'Cursor::at_': WEAVEC_BORROWED withdrawn (3 new errors), or 'const char *WEAVEC_ESCAPES p'  4 errors
no fix, fewest errors first:
  table/block.cc  4 errors: pointer-cast 3, raw-owner 1
  db/memtable.cc  9 errors: raw-owner 6, union-pointer 3
ready (marked or to mark WEAVEC_SAFE_FILE): 14 files
by rule: raw-owner 80, pointer-cast 61, owner-path 46
```

It also lists, with what safe functions trust, each likely bound the log
records on a member or a global that code which is not safe can write:
the member is not private, or its class has a member function or a
friend that is not safe. Those are the bounds that rest on T-A1 and V-A1
with nothing compared. `--format=json` gains `fixes`, `choices` and
`unchecked`, and `conversion.files` loses `rules` for a list of the
errors' places.

### 9. Explanations and guides

- `resources/safe/rules.md` gains a page `fix` (as `safe-cxx` is a page
  and not a rule): the three levels, what a note with a fix looks like,
  the fixer's loop, what to read in its log. `--weavec-explain fix`
  prints it. The pages of `index-single`, `call-args`, `escape`,
  `bad-annotation` and `thread-share` say which fix each error carries.
- `docs/pages/guides/convert-cxx.md`: the loop becomes survey, fix, read
  the choices, decide them, fix again, then the errors with no fix, then
  the tests in the checked build. `safe-c.md` and `safe-cxx.md`: the
  notes as they now read. `docs/pages/reference/cli.md`: `--weavec-fix`,
  the ledger's and the audit's versions.
- `docs/pages/reference/guarantees.md`: under T-A1 and V-A1, that a
  bound a fixer wrote is a bound, and where the audit lists it.

### 10. Cleanups and breaking changes

- **Breaking:** §4's compile error. The notes of the rules of §3,
  which tests and scripts that read them pin. The ledger's version 12
  without `survey` and `surveyGlobals`; the audit's version 8 and its
  plan's layout.
- **Removed:** the per-function survey counts and their second copy in
  the audit; the generic bound note ("'const T \*WEAVEC_COUNTED_BY(n)
  p'") wherever evidence gives the text.
- **Not changed:** every rule's verdict but §4's; what any pass does;
  the runtime and its ABI; `weavec.h`; a compile without errors, which
  proposes nothing and costs nothing.

### 11. Stages and gates

- **S0 Before code.**
  (a) The evidence of §3.2 prototyped outside the compiler, over the
  survey's five projects (yaml-cpp unedited with them): for how many
  wanted declarations is there evidence, how often does it agree, and,
  on tinyxml2 and yaml-cpp, how often is it the annotation the agents
  wrote. Under half of the wanted declarations with one answer means the
  kinds of evidence are wrong, and the design waits.
  (b) (D5) counted: on leveldb and pugixml, the errors left after
  `WEAVEC_BORROWED` first and after `WEAVEC_ESCAPES` first, and the
  locals each places.
  (c) Whether a safe function whose `WEAVEC_LIFETIMEBOUND` its body
  contradicts is rejected today.
  (d) The list of ownership's fixes for C.
  (e) §4 run over the tests, the cases, the documentation's examples
  and both conversions: what it newly rejects.
  (f) Recorded commands replayed for yaml-cpp's and leveldb's CMake
  builds: response files, precompiled headers, generated sources,
  dependency files.
  (g) The projects of G5 chosen, and their names and commits written
  down before any tool of this project is run on them.
- **S1** Errors as data: `report`, subjects, the ledger's `errors`,
  partial ledgers, the notes. **S2** The certain fixes and the marker.
  **S3** Evidence in one function and the likely bounds; §4. **S4**
  Wants across units; `WEAVEC_LIFETIMEBOUND`; (D5). **S5** The fixer:
  merge, apply, run again, withdraw, the log, `--dry-run`. **S6** The
  audit's plan, the `fix` page, the guides. **S7** The gates.

Gates **(D6)** (thresholds are proposals until S0; G4 is the one that
decides whether this RFC did what it was for):

- **G1 Tests.** `test/Safe/fix-bounds.cpp`, `fix-bounds.c`,
  `fix-escapes.cpp`, `fix-const.cpp`, `fix-borrowed.cpp` (each note's
  exact text and its edit under `-fdiagnostics-parseable-fixits`);
  a case in `test/Safe/cxx-bounds.cpp` for §4;
  `test/Driver/safe-fix.cpp` (two units and a shared header: the merge,
  three rounds, a withdrawal, the marker, `--dry-run`, a changed file
  refused, the log), `safe-fix-command.cpp` (the `--` form); the
  ledger's and the audit's versions; a unit test of the merge and of
  applying edits.
- **G2 What the fixer clears.** On the five projects unedited,
  `--weavec-fix` alone: at least half of the distinct errors gone, and
  four in five of `index-single`, `escape` and `call-args`; no project
  with more errors than before; at most one fix in twenty withdrawn.
- **G3 What the fixer writes is right.** For each declaration that both
  the fixer and RFC 0047's and RFC 0048's conversions of tinyxml2 and
  yaml-cpp annotated, the same annotation in at least 19 of 20; every
  disagreement listed with its evidence.
- **G4 The conversions, again.** tinyxml2 and yaml-cpp from their
  unedited sources, each by an agent that has not seen them, with the
  guide and the fixer: wall time at most 570 seconds for tinyxml2 (half
  of the Rust rewrite's 1,145) and 580 for yaml-cpp (half of its own
  1,161 in RFC 0047);
  the share of safe functions within two points of 93.5% and 99.3%;
  their tests passing in the checked and the fast build with no report.
  Recorded: rounds, the fixer's edits, the agent's edits.
- **G5 A project no RFC has read.** One C++ project of at least 50,000
  lines and one C project of at least 20,000, from S0 (g): the errors
  before and after the fixer, and, in two hours of an agent's time, the
  share of functions safe with the project's tests passing in the
  checked build. Recorded, with no threshold: it is the first
  measurement of its kind.
- **G6 Nothing believed is wrong.** Every converted project's tests
  built fast under `-fweavec-checks=verify`: no `weavec.proven`. Built
  checked: no report. The audit's list of unchecked bounds read, each
  one, for the two conversions.
- **G7 New code.** The `json` and the LRU ports written again by fresh
  agents: rounds and time beside RFC 0044's (3 rounds, 9 min 7 s; 2 min
  18 s). Not slower; fewer rounds expected, since the notes now say the
  text.
- **G8 The tool is safe to run.** A second `--weavec-fix` changes
  nothing; `--dry-run` changes nothing; each converted project compiles
  with plain Clang with no new diagnostic and passes its tests there.
- **G9 Nothing elsewhere.** A unit with no rule error compiles to main's
  object. A unit with errors compiles within 1.1 times main's time. The
  `dev` tests, the cases, the corpus gate and the documentation's
  examples pass, but for what §4 rejects, which S0 (e) lists; the
  Linux container's `ci-release` passes.
- **H1 Hygiene.** The library within 29,300 lines; the runtime
  unchanged.

## Annotation surface

None. `resources/include/weavec.h` stays at version 3.4. The fixer writes
the annotations that exist, in the places RFC 0041, RFC 0047 and RFC 0048
say they are written.

## Diagnostics

No rule identifier and no report kind is added.

**Notes, changed**, for an error that carries a fix (each pinned in
`test/Safe/`):

```text
note: fix (certain): declare it '<declaration text>'[, here and at <file>:<line>:<col>][; <consequence>]
note: fix (likely): declare it '<declaration text>'; <evidence> at line <n>
note: fix (likely): declare the member '<text>', a view's; or (certain) the parameter '<text>' if '<class>' objects are kept
note: choice: <first>; or <second>
note: <k> of <n> errors carry a fix; '<driver> --weavec-fix -- <this command>' applies them
```

**An error, in a new place** (§4), with its existing wording, pinned in
`test/Safe/cxx-bounds.cpp`:

```text
error: '<p>' points to one object, and field '<member>' has a bound [weavec-safe:call-args]
```

**The fixer's lines** (standard output; pinned in
`test/Driver/safe-fix.cpp`), as in §6, and its errors:

```text
weavec: fix: '<file>' changed since '<ledger>' was written; build again with -fweavec-safe-survey
weavec: fix: '<ledger>' records no command; it was not written under -fweavec-safe-survey
weavec: fix: the rules could not be run for '<unit>': <the compiler's first error>
```

The audit's plan reads as in §8.

## Drawbacks

- **Machine-written bounds at scale.** _Soundness_ says where they are
  not compared. A likely bound that is wrong in code the tests do not
  reach, on a member that only guarded code fills, is believed. A person
  could make the same mistake; a tool makes it without reading.
- **Annotations nobody chose.** A converted file may carry hundreds of
  tokens its authors did not write and may not want in that form (a
  `std::span` would say it better). The fixer picks the edit that
  changes no signature for a caller that compiles without WeaveC, every
  time, and that is not always the best conversion.
- **(D5) guesses a design.** Whether a class is a view is its author's
  decision. The guess can be withdrawn only when its damage shows in a
  unit the fixer can see.
- **A second way for the compiler to be wrong.** A fix that does not
  clear its error, or an edit that breaks a declaration, is a bug of a
  new kind, and each is a regression someone meets in the middle of a
  conversion.
- **The driver grows a tool.** Recorded command lines, replay, parallel
  jobs and file hashes are build-system work inside a compiler, with the
  corners build systems have (S0 (f)).
- **It may not be where the time goes.** The agents' records attribute
  rounds to the errors this clears, but tinyxml2's agent also spent its
  time on rewrites no fixer does (in-place parsing, the arena, `const`
  getters that normalize). G4 could fail with G2 passing.
- **The speed gates stay failed** for another RFC.
- **About 1,700 more library lines** and a limit raised a ninth time.

## Alternatives

- **Do nothing.** The loop is at parity, and parity with a slower result
  is the case against choosing it.
- **Better notes only**: say the text, apply nothing. It is S1 to S3 of
  this RFC and worth having alone, but the rounds remain: each layer of
  callers appears after the last was edited, at an agent's minute a
  round where the compiler needs a fifth of a second.
- **Clang's `-fixit`.** It rewrites files from inside each compile, so
  two parallel jobs rewrite one header; it applies only what is attached
  to an error; it cannot merge what two units say or take anything back.
- **Infer inside the compiler and write nothing**: bounds the passes
  believe without a declaration. It is what RFCs 0001 to 0034 did.
  Nothing a reviewer can read, facts that change when another file
  changes, and an analysis across functions to keep sound.
- **Whole-program inference that writes** (Checked C's 3C, CCured's
  kinds). One global solution in place of rounds. It needs the whole
  program at once and a constraint system beside the rules; the rounds
  reach the same closure through declarations, with the rules as the
  only judge. S0 (a) says how far short of it local evidence falls.
- **Leave it to the agent, and ship a prompt.** An agent can do all of
  this, and did, at 1,201 seconds. What a deterministic tool does in
  seconds should not cost tokens, and it should give the same answer
  twice.
- **Evidence from a run**: the checked build knows how large every
  object behind a pointer was. A profile could propose bounds no static
  evidence shows. It needs a test run before the first fix; _Future
  work_.
- **The speed work first**: the allocator's inline paths, loop ranges,
  views (RFC 0048's amendments 8 and 12). It moves 1.44 toward 1.25 and
  leaves a conversion costing what a rewrite costs. It is owed, and it
  needs no decision this RFC would wait for.
- **A project tool in the manner of Cargo.** RFC 0048 said it: a product
  of its own.

## Prior art

- **`rustc` and `cargo fix`.** A suggestion is a span, a replacement and
  an `Applicability` (`MachineApplicable`, `MaybeIncorrect`,
  `HasPlaceholders`, `Unspecified`); `cargo fix` applies the first kind,
  compiles again, repeats a few times, and backs its changes out when
  the result does not compile. Editions are migrated with it. The levels
  of (D1) are those, renamed for what they mean here; the loop and the
  withdrawal of §6 are `cargo fix`'s, extended to likely fixes because a
  wrong one is caught by the rules and not by luck.
- **Clang's fix-its** (`FixItHint`; the internals manual's rule that a
  fix-it on an error must be the obvious and only fix, and anything else
  goes on a note). §2 follows the rule, which is why `-fixit` applies
  nothing here.
- **Clang's `-Wunsafe-buffer-usage`** and its fix-its to `std::span`: a
  declaration and its uses changed together, variables grouped when one
  cannot change without the other. It rewrites types, which changes
  signatures; this RFC annotates them, which does not. Its grouping is
  what a later `raw-owner` fix would need.
- **Checked C's 3C** (Machiry et al., OOPSLA 2022): converts C by
  solving for each pointer's kind over the whole program, infers bounds
  from heuristics much like (E1), (E2) and (E6), and reports the "root
  causes" that keep the most pointers unchecked, for a person to fix
  first. The plan's "choices, most errors first" is that report. Not
  taken: the global solve.
- **CCured** (Necula et al., POPL 2002): inferred kinds (`SAFE`, `SEQ`,
  `WILD`) and made the run-time checks follow them, so that a wrong
  inference cost speed and not safety. Here a wrong bound costs a false
  trap where it is compared, and safety only where T-A1 is leaned on,
  which is why the audit lists those.
- **TypeScript's "infer from usage"**: a declaration's type proposed
  from the uses in its function, offered as an edit. The same bargain as
  §3.2: local, cheap, often right, checked afterwards.
- **C2Rust and the ownership lifters after it** (Laertes, CROWN): the
  mechanical half of a rewrite into Rust, whose output is `unsafe` Rust
  that analyses then try to make safe. They start from a translation and
  recover the design; a conversion in place starts from the design.
- **This project's `escapes.py`**: the prototype, with a regular
  expression where the interface should be.

## Unresolved questions

1. **How often does local evidence agree, and is it right?** S0 (a).
   Everything else depends on it.
2. **(D5).** Which first fix leaves fewer errors and fewer placed
   locals on code that was not written for it. S0 (b) may reverse the
   default or make both a choice.
3. **Is withdrawal by subjects enough?** A likely bound can surface as
   an error whose subjects do not include it (a caller two calls away).
   Is the fallback to withdraw the round's likely fixes on every subject
   of the new error, or to leave it and list it?
4. **Templates.** Evidence comes from instantiations and may differ
   between them; a fix lands on the template. All instantiations
   agreeing is the proposal; is that too strict for a `T *` whose count
   is the same member in each?
5. **Virtual functions.** `WEAVEC_ESCAPES` must be on every override. A
   unit sees some of the hierarchy; the rest arrives as `bad-annotation`
   a round later. Does any hierarchy in the corpus make that oscillate
   with a withdrawal?
6. **Headers read under two languages or two standards**, and units the
   ledgers do not record (a test binary not built in the survey). The
   fixer edits a header for the units it knows.
7. **Generated sources and vendored code** inside the project's tree.
   `--only` is the answer today; should the default exclude a file under
   a unit's build directory?
8. **Is a recorded command line acceptable in a ledger?** It can hold
   definitions a team does not want in a file. It is written only under
   the survey; is that enough, or does it need its own flag?
9. **What a note should say when the fix is in another file** the reader
   may not own (a dependency's header that is not a system header).
10. **Does §4 reject a real idiom?** S0 (e).
11. **Round 4 of the probe is not this RFC's to fix, and someone must.**
    A safe function passes `text.data() + 2`, a pointer into a local
    `std::string`, for a `WEAVEC_ESCAPES` parameter. Nothing is said when
    it compiles; when it runs it stops or not by the string's length. RFC
    0048's `Kept` follows `&`, members and subscripts of a local, not the
    result of its `data()`. Should that be `escape` at compile time, or
    should the string be placed? Until it is decided, (D5) keeps the
    fixer from writing the annotation that leads there for views, and
    nothing keeps a person from writing it.

## Future work

- **Fixes that rewrite**: a raw owner to `std::unique_ptr` with its
  `new`, `delete` and uses; a `const_cast` forwarder to a loop; a list
  iterator kept across `erase`. Each is a group of edits with a design
  in it, and wants its own evidence.
- **Evidence from a run** of the checked build (see _Alternatives_).
- **The fast mode per function**, so that what the fixer converts is
  fast before the whole unit is (RFC 0047's list).
- **The speed gates**: RFC 0048's G5, G7 and G8.
- **An editor**: the same fixes through a language server.
- **A large conversion as a standing gate**, once G5 has been run once.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what the gates found. They override the body.

1. **Keys (§1).** A declaration is named by its file and its qualified
   name: a function's with its canonical type (overloads), a parameter's
   with its function's and its position, a static local's with its
   function's; a template's for an instantiation's. The design's
   `<file>:<line>:<column>` moved with every edit the fixer made, so that
   no error of a later round named a declaration a fix of an earlier one
   changed, and nothing was withdrawn.
2. **Wants (§1).** A rule says what it wants just before it reports, at
   the same place (`FixPlanner::want`); the report takes it. The error
   sites of `index-single`, `call-args` (an argument, a store or a return
   into a bound, a position, a C library call's count), `escape` (C and
   C++, `this` included), `bad-annotation` (declarations and overrides),
   `thread-share` (globals) and `owner-path` (the aliased choice) say one.
   `string-index` and `string-write` say what they read, with no fix, so
   that the fixer can withdraw a string bound that brought one (amendment
   6). An error of the C++ flow (`call-alias`, `stale-borrow`) with no want
   gets E7 from its message: the local it names first, given a call's
   result whose callee returns one parameter on every return.
3. **The evidence (§3.2), as built.** E1 reads a C library row's
   `bytes(aN)` (not a `min` with a string's length, as `strncmp`'s), a
   library class's `counted` and `ended` clauses, and a project
   function's parameter that states a bound. E2 reads `i < n`, `n > i` and
   `i != n` in the condition of a `for`, a `while`, an `if` (its `then`), a
   `?:` (its true side) or the left of `&&`, around the index or the
   offset. E3 reads `p < e`, `e > p`, and `p != e` only as (a conjunct of)
   a loop's condition: `if (p == q) return true;` in tinyxml2's
   `StringEqual` made both parameters each other's end. E4 is not proposed
   for a pointer whose function writes through it (a byte assigned,
   stepped or its address taken): safe code only reads a string, and
   tinyxml2's and pugixml's in-place parsers were fixed into string
   errors. A value set from a literal is weak evidence, counted only
   without strong. E5 holds only when nothing else indexes or steps the
   pointer. E6 also reads a member set from `new T[n]` beside a member set
   from `n`. A local's evidence is its own and that of the locals that
   copy it (E2 and E5 only through a copy that is not stepped). A count
   that names the declaration itself is not evidence.
4. **Locals (§3.2).** A local's want goes to the declaration every value
   it is given comes from (through steps); a local given values from two
   has no fix. snappy's and pugixml's cursors (`ip`, `op`, `s`) are such
   locals: most of their `index-single` errors have no fix.
5. **(D5) as built.** A class of plain values has no user-declared
   destructor, copy or move, no base, no virtual function, and members
   that are scalars, references, `std::basic_string_view` or `std::span`.
   The note reads as designed.
6. **Withdrawal (§6).** By subjects, as designed (with amendment 1's
   keys). Unresolved question 3: a member made `WEAVEC_BORROWED` is also
   withdrawn by a new error with no fix that quotes the member's or its
   class's name, since the rules of views do not name the member; any
   other likely fix only by subjects (quoting names withdrew every fix on
   a parameter named `p`). A round that breaks a compile (an error that is
   not a rule's in a re-run) is taken out whole, with the includes it
   added, and the files it broke are kept in `weavec-fix/broken-<round>/`;
   a round whose compiles cannot be started is taken out and the fixer
   stops. An include the fixer added to a file where no fix of its is left
   is taken out. What was withdrawn is read back from the log by the next
   run in the same place, so a second run does not try it again.
7. **Where the fixer works (§6).** Beside the ledgers' directory
   (`<parent>/weavec-fix/`), absolute, so that neither the fixer nor the
   audit reads its runs as the project's ledgers: `run-<n>/` (each re-run's
   ledgers and each compile's output), `wants.txt`, `weavec-fix.json`, and
   `ledger/fixed.ledger.json`, every unit as the rules last left it, which
   the audit reads. The `--` form works in a temporary directory. The
   re-runs use the fixer's own executable (`--driver-mode=g++` for a C++
   command) in each unit's recorded directory, without `-o`, `-c`, `-S`,
   `-E`, `-M*`, `-fweavec-ledger=`, and with `-fsyntax-only`,
   `-fweavec-safe-survey`, `-fno-weavec-summary`, a ledger of their own and
   `-fweavec-fix-wants=<file>` (§5's option is this hidden driver flag, not
   a `cc1` one).
8. **Includes and the mark (C5).** `#include <weavec.h>` and the mark go
   after the file's last `#include` outside every conditional but an
   include guard (the file's first directive, `#ifndef`), before the
   file's first edit; without one, after the guard's `#define` or `#pragma
   once`, or at the start. libzmq's `zmq.h` first placed the include in a
   Windows block, and yaml-cpp's `dragonbox.h`, whose includes are all
   inside `#if`s, got its mark in a dead `#elif` (150 functions silently
   not safe, while the audit called the file ready).
   Whether a file already includes `weavec.h` or says `WEAVEC_SAFE_FILE` is
   read from its directives and lines, not its comments.
9. **Edits (§1).** An annotation after a `*` or `&` is ` X` before a space
   and `X ` otherwise; `WEAVEC_LIFETIMEBOUND` follows a parameter's name and
   an array's bound after it, and a declaration that leaves the parameter
   unnamed has no place for it, so the fix is not proposed; `WEAVEC_ESCAPES`
   for `this` follows `const`, `volatile`, `&`, `&&` and the exception
   specification, a macro's (`PUGIXML_NOEXCEPT`) included. `const` for
   (C4) goes after the `*` of a pointer and before the type otherwise.
10. **Notes (§2).** The places of a certain fix's other declarations are
    named by their presumed file, line and column; (C2)'s note lists what
    it writes (`fix (certain): write WEAVEC_COUNTED_BY(count) on
    'values'`). Clang prints fix-its for `-fdiagnostics-parseable-fixits`
    only with caret diagnostics on.
11. **The ledger (§7).** Every rule error is recorded, printed or
    surveyed. A function that is not safe and was surveyed has
    `"surveyed": true`. A unit that does not reach code generation writes a
    partial ledger named `<source>.<hash of its path>.ledger.json` in a
    directory, and prints no summary line.
12. **The audit (§8).** `unchecked` lists every bound on a member that
    code which is not safe can write, not only the fixer's: the ledger
    marks a counted member that is not private, or whose class has
    friends, with a field row of kind `writable`, and the audit adds the
    counted members of classes with a member function that is not safe.
    The plan lists the files whose errors include one with no fix, with
    those errors' rules.
13. **Safe C (§3.5).** C gets (C1) for a parameter stored where it
    outlives the call or passed to an escaping parameter, E7 for a
    returned parameter, (C2), and the bounds. Ownership's notes were not
    turned into fixes: each names an owner whose evidence is the function's
    whole flow, not one use.
14. **Line limit (D6).** The planner (`SafeFix.cpp`, ~2,000 lines) and the
    fixer (`Fixer.cpp`, ~1,000) are about 3,000 lines where the design
    counted 1,700 for everything; the library is 31,490 lines and the limit
    is raised to that, a departure from (D6)'s 29,300 that the owner
    accepted on 2026-11-18.
15. **From the conversions (G4).** Three of the fixer's mistakes the agents
    reverted are fixed: a count from the allocation (`new T[n]` through a
    local, or an array a constructor sets the member to, beside a member set
    from the same `n` or `N`) is *exact* evidence and outranks every other,
    since a comparison or a call bounds only what is read (`DynArray::_mem`
    was `WEAVEC_COUNTED_BY(_size)`, which makes every `Push` out of bounds;
    it is now `_allocated`); E3 holds only in a loop's condition, and `!=`
    only for a pointer the function steps forward and never back
    (`ConvertToChars` writes backward from `end`, and got `ENDED_BY(begin)`
    on it); an agreement (C2) is keyed by what it writes too, so that a
    base agrees with each of its overrides (tinyxml2's `ParseDeep`: the
    base's certain fix was never written). Two are the design's and stay:
    `WEAVEC_ESCAPES` spreads up from `SetValue(str, staticMem)`, which keeps
    `str` only when `staticMem`, to eleven functions that only copy (the
    rule's verdict, which the agent answered by routing the copies around
    it); and (D5) made GraphBuilderAdapter's frames views, which thirteen
    errors that quote neither the member nor the class then rejected.
16. **Compile time (G9).** The planner's index of uses skips system
    headers and reads each body once, and a declaration's bound is planned
    once however many errors want it.
17. **Ledger names (§7), from G5.** In a ledger directory the first object
    of a name keeps `<object>.ledger.json` (it claims the name as it is
    written); another object of the same name, from another directory,
    gets `<object>.<hash of its path>.ledger.json`. nng's six pairs of
    same-named sources (`pair0/pair.c`, `pair1/pair.c`) overwrote each
    other's ledgers, so six files were never surveyed, fixed or marked.
    A ledger written with no object (`-fsyntax-only`, as every one of the
    fixer's runs is) is named by its source and a hash of the source's
    *absolute* path: with the path as written, `a/x.c` and `b/x.c`
    compiled in their own directories shared one.
18. **From review of the fixer.** Merging reads every unit's proposals
    before any unit's evidence (§5), so what a unit's evidence says does
    not depend on the order of the units; the edits of several units are
    written once, with the include if any of them needs it (a unit that
    saw `weavec.h` before a header needs none, one that did not does); and
    two facts for one declaration are a choice even when both are certain.
    Before a round writes anything every file it edits is checked against
    its ledger, and a changed one stops the fixer (§6 step 2); a file was
    skipped and the rest of the round written. A round that breaks a
    compile is taken out: its likely fixes are withdrawn, its certain ones
    are tried again without them, and only a round of certain fixes that
    breaks a compile alone has them withdrawn (the one case that departs
    from step 5, which said certain fixes are never withdrawn; without it
    the fixer would not stop). Every exit after the first round writes
    `weavec-fix.json`, with `status` and the `choices` left. Each run
    empties its `run-N` directories first, the fixer's directory is
    beside the ledgers' directory for a ledger operand too, and neither
    the fixer nor the audit reads a `weavec-fix` directory under one it
    walks. An edit is an insertion only (§1's length is gone: the fixer
    never wrote one). G2 and G3 measured again after it: unchanged.
19. **Round 1 sees no other unit's evidence.** The survey a fixer reads
    was compiled without the wants of §5 (no one knew them yet), so the
    first round's likely fixes are merged from the errors alone, and
    evidence from a unit with no error on a declaration arrives with that
    round's runs, after the fix is written. Nothing written is believed,
    so a wrong bound is an error or a trap, not a hole; holding back the
    likely fixes on a header's declarations to round 2, or surveying once
    with the wants, is future work.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset) on
2026-11-17 and 2026-11-18, against main at 392b7201. The evidence is in
`build/rfc49/` (not committed; `README.txt` there): `g2/` (`setup.sh`,
`survey.sh`, `g2.sh`, `gate.py`), `g3/agree.py`, `g4/` (the two
conversions, each with `record.md`, `log.md`, `orig/` and the fixer's
log), `g5/` (the two unseen projects, their commits in `projects.txt`),
`g6/run.sh`, `g7/` (the ports), `g9/` (`g9.py`, `time.py`). The agents of
G4 and G7 worked from the guides and `--weavec-explain` with frozen copies
of the compiler (`tc/`, `tc2/`); G4's ran before amendments 15 and 16.

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass.** `test/Safe/fix-bounds.cpp`, `fix-bounds.c`, `fix-escapes.cpp` (C1 on parameters and `this` after `noexcept`, D5, C4, C2, the aliased choice), `notes.c` and `declarations.c` (the notes as they read now), §4 in `fix-bounds.cpp`; `test/Driver/safe-fix.cpp` (two units and a header: the dry run, rounds, a withdrawal, the marks, a second run that applies nothing, a stale file refused, the log), `safe-fix-command.cpp` (the `--` form), `safe-survey.cpp` (ledger 12, audit 8); `unittests/Frontend/RuleFixTest.cpp`. `ctest --preset dev` passes 52 of 52; lit 149 of 149. |
| G2 What the fixer clears | **Fail.** The five projects, unedited (`gate.py`): 2,457 distinct errors become 2,035 (17%; the gate asks half); `index-single`, `escape` and `call-args` 1,504 become 1,091 (27%; it asks four in five); no project has more errors than before; 69 of the 663 fixes written were withdrawn (10%; it asks at most 5%). By project: snappy 343 to 332, tinyxml2 244 to 207, yaml-cpp 169 to 151, pugixml 882 to 686, leveldb 819 to 659, each in at most 16 s; measured again after amendment 18, the same. What is left is not an annotation: owners to rewrite (`raw-owner`), classes to declare aliased (a choice), casts and unions, cursors that are locals given values from two places (snappy's `ip`, `op`), and in-place parsers whose strings are written (tinyxml2, pugixml), whose string bounds are withdrawn as designed. |
| G3 What the fixer writes is right | **Pass.** Every declaration both the fixer and RFC 0047's or RFC 0048's conversion annotated got the same annotation: tinyxml2 34 of 34, yaml-cpp 28 of 28 (`agree.py`), and again after amendment 18. |
| G4 The conversions, again | **Fail.** tinyxml2 in 1,015 s (the gate asks 570; RFC 0047's took 1,201), 17 rounds, two of them fixer runs, 403 of 419 functions safe (96.2%; 93.5% before), 517 of 517 checks in both builds. yaml-cpp in 1,341 s (it asks 580; 1,161 before), 21 rounds, two fixer runs, 907 of 912 safe (99.45%; 99.3% before), 1,011 tests in both builds. Both reached a higher share than before, under RFC 0048's stricter rules, but not faster: the agents spent their time on what has no fix (tinyxml2's arena and in-place strings, yaml-cpp's `const` twins and the graph builder) and on reading and correcting the fixer's edits; yaml-cpp's first fixer run failed (a relative ledger path, fixed after, amendment 7). Both found the library defects RFC 0047's found, and tinyxml2's a new one (`XMLPrinter::Write` narrows a `size_t` to `int` for `PushArr`). |
| G5 Projects no RFC has read | **Recorded.** libzmq 4.3.5 (C++, ~50,000 lines) and nng 1.9.0 (C, ~65,000), chosen and their commits written down before any tool ran. The fixer alone: libzmq 2,299 to 2,055 errors (95 s), nng 1,993 to 1,841 (11 s); their errors are mostly casts, unions, owners and `owner-path`. Two hours of an agent's time each (with the compiler before amendment 17), both stopping early: **libzmq** 0 to 1,275 of 1,568 functions safe (81%) in 73 minutes, its checked build's tests as at the start (114 of 121; 3 network tests fail on the unconverted library too, 4 fuzzers not built); **nng** 0 to 1,778 of 2,917 (61%; 1,144 of its original 2,152 safe themselves, 1,775 with their bodies in a safe typed function the agent split out of each `void *` callback) in 78 minutes, 85 of 86 tests (one needs the network). Both found real defects: libzmq's proxy `memcmp`s six bytes of a five-byte command; nng's `getsockopt(LOCAL_PEERPID)` is given 76 bytes for a 4-byte `pid_t`, and `nni_msgq_resize` reads one slot past its queue. What cost them most: likely bounds too small for cursors whose count grows (66 tests trapped until reverted, as _Soundness_ says a too-small bound does), `WEAVEC_ESCAPES` that is right for the rules but stops tests that pass stack objects (`stack-escape`), C library calls on pointers without a bound that compile and trap when they run (Safe C checks them then, RFC 0042), and the ledger collision of amendment 17. The fixer added `#include <weavec.h>` to libzmq's public `zmq.h`, which its users then need. |
| G6 Nothing believed is wrong | **Pass.** Both conversions' tests built fast under `-fweavec-checks=verify`: no `weavec.proven`, no report (tinyxml2 517 checks, yaml-cpp 1,011 tests); built checked: no report. The audit's unchecked bounds were not read one by one. |
| G7 New code | **Recorded.** The `json` port by a fresh agent: 2 rounds to a clean build (3 before), 5 min 1 s to `check.py`'s result (9 min 7 s before), 645 of 646 as before; it did not need the fixer. The LRU port: 1 round (as before), 3 min 10 s (2 min 18 s before); it kept indices and met no error. One false `stale-borrow` (RFC 0044's: a member vector of a local struct erased then pushed) is in its record. |
| G8 The tool is safe to run | **Pass.** A second `--weavec-fix` applies nothing (`safe-fix.cpp`), `--dry-run` writes nothing, and both conversions compile with plain Clang (C++17, as their agents built them) with no new diagnostic and pass their tests. |
| G9 Nothing elsewhere | **Pass.** 2,066 compiles (RFC 0044's and RFC 0046's sets, `test/cases`, `test/Guards`; guarded, fast and checked) give the same object with main's compiler and this one; 1,558 fail with both (the seeded bugs) and none with one only, so §4 newly rejects nothing there. Survey compiles of units with errors take 0.98 to 1.12 times main's (`time.py`, after amendment 16; 1.37 before it). The `dev` tests and the documentation's examples pass; the Linux container's `ci-release` (libstdc++, aarch64, warnings as errors) passes 52 of 52. |
| H1 Hygiene | **Pass at the raised limit** (amendment 14, accepted by the owner): library 31,490 lines; runtime unchanged. |

What the numbers say about the owner's goal: the compiler now says what to
write, and a whole project's mechanical annotations come from one command
that agrees with what people wrote every time the two met. It did not make
a conversion cost half of a rewrite: on these libraries most of an agent's
time goes to what no annotation fixes, and the fixer's mistakes cost time
to read back. The next change toward the goal is the fixes that rewrite
(_Future work_) for the errors that are left: owners, `const` twins,
in-place strings.
