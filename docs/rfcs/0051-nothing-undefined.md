# RFC 0051: Nothing undefined

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-11-20
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Adds a family of guarantees, (N1) to
  (N6), beside (W), (O), (X), (Y), (Z) and (V) of
  [RFC 0042](0042-typed-memory.md) to [RFC 0050](0050-checked-once.md),
  and supersedes none of them. Supersedes, for safe functions only, the
  limit of 4,096 bytes on zeroed locals
  ([RFC 0035](0035-guard-by-default.md) §1), the sentence "Nothing of it
  rests on anything but the rules" wherever the guides say it without
  naming what else a compile assumes, and rule 2 of `AGENTS.md` for four
  of Clang's own diagnostics (D5). Extends `pointer-bytes` and
  `union-pointer` ([RFC 0041](0041-safe-c.md), [RFC 0044](0044-safe-cxx.md))
  to `bool`. Adds two rule identifiers, one link-time error, five report
  kinds and one trait of the library table. The ledger becomes version 13
  and the runtime's ABI 47.

This RFC was written at the request of the project owner on 2026-11-20,
who asked an eleventh time for the single change that would do most to
make an AI rewrite into C or C++ as attractive as an AI rewrite into
Rust, to be implemented next, with breaking changes and the removal of
old code welcome. The measurements in _Motivation_ were taken on main at
`2d56b9f1` (RFC 0050 merged), darwin-arm64, the `dev` build, Homebrew
LLVM 23.1.0. The probes, the two trial programs, their conversions and
the scripts are in `build/rfc51/` (not committed). The design was written
before any of it was built; the _Implementation amendments_ and the
_Measured gates_ at the end record how it was built and what it measured.

The owner accepted the design as drafted on 2026-11-20, before any of it
was built, and with it the eight decisions that were theirs to make. They
are marked **(D1)** to **(D8)** where they are argued, and collected
here. The thresholds of §12 and the _Unresolved questions_ are still
open, and S0 is where they are settled: by (D1) a row of the list that
fits no disposition comes back to the owner, and by (D8) so does a part
that costs more than its bound.

- **(D1) The guarantee is a closed list, and the list is the gate.** A
  safe function has no behavior the language leaves undefined, other than
  what (X6) and (Y) already say of a stale pointer. Every item of the C
  standard's list of undefined behavior (C23 Annex J.2) and of the C++
  standard's core list gets a row that says which rule, definition, stop
  or assumption disposes of it, and a test. This RFC is not Implemented
  while a row says "undefined".
- **(D2) Arithmetic is defined, not stopped.** In a safe function signed
  overflow wraps, a shift count is taken modulo the width, and a
  conversion from floating point to an integer saturates, in both builds.
  Division by zero stops.
- **(D3) Code declared unreachable stops.** `std::unreachable()`,
  `__builtin_unreachable()` and the end of a function that must return a
  value are stops with a report, in both builds. The optimizer loses the
  hint.
- **(D4) What only asserts is an error, and its fix is to delete it.**
  `[[assume]]`, `__restrict`, `[[gnu::pure]]` and their kin, and the
  command-line flags that tell the optimizer what it may believe, are not
  accepted in safe code.
- **(D5) Four of Clang's warnings are errors in a safe unit, under
  Clang's names.** A model already knows what `-Wreturn-type` says. They
  are the first compile-time diagnostics of a safe unit that do not end
  in `[weavec-safe:<rule>]`.
- **(D6) Bytes do not become a `bool`, and a signal handler is registered
  by code that is not safe.** Two rejections that natural code meets: one
  of the two trial programs registers a handler.
- **(D7) Safe units agree when they are linked.** Each object from a
  safe unit carries a record of the types, declarations and globals it
  believes, and `weavec-cc` refuses to link units that disagree. It is the
  first check that is not decided inside one function or one unit. It is
  a comparison, not an analysis.
- **(D8) The cost is bounded before anything is kept.** Every program
  this project times runs within 1.03 times its fast build on main, or
  the part that costs more is reported and comes back to the owner.

## Summary

Ten RFCs gave C and C++ rules an agent can iterate on, fixes the compiler
writes, and a fast build within a fifth of Clang's on new code. All of it
is there to support one sentence, the one Rust's case for generated code
rests on: **what compiles as safe cannot corrupt memory, whoever wrote
it and whether or not anyone read it.** WeaveC cannot say that sentence
today, and not because of a hole in a rule.

Measured on main: seven programs whose every function is safe, compiled
with no error and no warning, that read outside their objects, write
into another object, or jump through a table without a range check. Five
do so in the checked build too. One returns from an exhaustive `switch`
over an `enum class`; one calls `std::unreachable()` after it, as C++23
code does; one reads a file header into a struct with a `bool` in it;
one has two files that each define a helper `struct Entry`; one has a
local larger than its thread's stack. None of them breaks a rule. Each
uses behavior the language leaves undefined and no rule is about, and
the optimizer does the rest.

Rust has no such programs because its definition of safe is not a list
of memory rules. It is the absence of undefined behavior: arithmetic
wraps or panics, `unreachable!()` panics, a `bool` cannot be made from a
byte, a function cannot end without its value, two modules cannot
disagree about a type. This RFC gives safe C and safe C++ the same
definition and closes the list. Each undefined behavior a safe function
can still reach gets exactly one of four dispositions:

- **Defined.** Signed arithmetic wraps, shifts and float conversions have
  one result, loops may run forever, every local starts as zeroes (D2).
- **Stopped.** Unreachable code, a missing return value, division by
  zero and a full stack end the program with a report, in both builds
  (D2, D3).
- **Rejected.** Assertions the optimizer would believe, bytes copied
  into a `bool`, a signal handler registered from safe code, and four
  mistakes Clang already warns about do not compile (D4 to D6).
- **Compared.** The types, declarations and globals that safe units
  share are compared when they are linked (D7).

Measured before anything was built: the definitions and the stops
together cost the `json` port 1% of its run time under Clang, and the
definitions alone 1% under WeaveC's fast build. Natural code is barely
touched: two agents wrote 7,354 lines for this RFC with no checker in
mind, two more made them safe in 6 and 13 minutes, and a reading of the
result finds one thing this RFC would reject, the registration of a
signal handler.

Expected result (gates, §12): every probe of `build/rfc51/probe` fails
to compile, fails to link or stops with a report in both builds; every
row of the list has a test; no program runs more than 3% slower; and the
guarantees page states, for the first time, a property of a whole safe
program in one sentence.

## Motivation

### The sentence the project cannot say

RFC 0050's closing table has two columns the earlier RFCs worked on, the
agent's time and the program's cost. There is a third that no gate has
measured since RFC 0044 seeded bugs into safe C++: whether the result
can be trusted without being read. That column is why an AI rewrite
into Rust is attractive at all. Nobody reads 100,000 generated lines.
What makes them acceptable is that `rustc` took them, and that
`#![forbid(unsafe_code)]` plus a clean build is a statement about every
execution.

WeaveC's statement today is the guarantees page: (W1) to (W5), (O1) to
(O5), (X1) to (X6), (Y1) to (Y6), (Z1) to (Z6), (V1) to (V9), each a
property of pointers, bounds, owners, borrows, types or threads. They
were written by asking what a rule or a check establishes. None was
written by asking what else the compiler assumes when it compiles a safe
function. The compiler assumes a great deal: that signed arithmetic does
not overflow, that a function returns before its closing brace, that
code marked unreachable is not reached, that a `bool` holds 0 or 1, that
every file means the same thing by a name. When one of those is false
the optimizer is entitled to do anything, and the checks the fast build
rests on (the library's comparison of an index with a size, RFC 0050)
are ordinary code it may then remove.

### Measured: safe programs that are not

`build/rfc51/probe/` holds each program below. Each is compiled with
`weavec-c++ -std=c++23 -O2 -fweavec-safe`, with every function safe, no
error and (but where noted) no warning. "Checked" is
`-fweavec-safe-checks=full` at `-O2`.

| Probe | The source | Fast build | Checked build |
| --- | --- | --- | --- |
| `ub7.cpp` | `long step(Op op, long x)` is a `switch` over the ten values of `enum class Op`, each case returning; the opcode is `static_cast<Op>` of a number from the command line | opcode 40 prints 40 and opcode 1000 prints 1, with no stop: the switch's table is indexed without a range check | opcode 1000 runs another case's code (`std::out_of_range` from a `vector::at` the opcode did not select) |
| `ub8.cpp` | `indexOf(Color)` is a `switch` followed by `std::unreachable()`; its result indexes a `std::array<long, 3>` | colors 7, 12, 100 and 100000 print a value and exit 0 | color 100 prints `5ec2e7`, the member after the array |
| `ub3.cpp` | `[[assume(i < 8)]]`, and `if (i >= 8) __builtin_unreachable();`, each before `cells[i]` on a `std::array<long, 8>` | index 8 prints the member after the array, twice | the same |
| `more/boolbytes.cpp` | `std::fread(&header, sizeof header, 1, file)` into `struct Header { unsigned length; bool compressed; }`, then `sizes[header.compressed]` on a `std::array<long, 2>`; the file's byte is 2 | prints the member after the array | the same |
| `odr/` | `cache.cpp` and `index.cpp` each define their own `struct Entry` (8 bytes, and 56) with a member function `touch()` | `balances sum 1094795985 (expected 400)`: one file's constructor ran over the other's vector | stops: `heap-buffer-overflow` |
| `odr/decl_*.cpp` | `extern const long limits[16];` in one file, `const long limits[4]` in the other; `i < 16 ? limits[i] : -1` | index 12 reads 96 bytes past the array and prints | stops: `global-buffer-overflow` |
| `more/clash1.cpp`, `clash.cpp` | a thread calls a function with a local `std::array<long, 100000>` (800 KiB; a thread's stack here is 512 KiB) and writes elements of it | the frame is made with one subtraction and no probe (`clash.s`); in `clash1.cpp` a write 800 KiB below the stack's top lands in mapped memory and the program exits 0 | `clash1.cpp` exits 0 as well; `clash.cpp`, which writes a run of elements, built at `-O0`, gets WeaveC's own report of where they land: `stack-buffer-overflow ... 0 bytes after 'mine' ... in the frame of victim(...)`, another thread's local |

The first four are wrong in both builds, because nothing the checked
build adds is involved: the optimizer removed the library's comparison,
or the range check of a jump table (`ub7.s`: the indirect branch has no
comparison before it), and the access stays inside the heap block or
the frame the shadow describes. The two of `odr/` are caught by the
checked build's shadow and are silent in the build that is shipped, and
the last is caught only when its writes happen to touch a redzone.

Clang says nothing about any of them. Its `-Wreturn-type` deliberately
accepts a `switch` that names every enumerator, and an `enum class` may
still hold any value of its underlying type, so `ub7.cpp` is the idiom
and not a slip.

Six more compile as safe and do something the standard does not define,
without a memory error that these probes could show:

| Probe | What happens today |
| --- | --- |
| `more/arith.cpp` | `7 / 0` is 0 and `INT_MIN / -1` is `INT_MIN` on arm64 (both trap on x86-64); `1 << 40` is 256; `a + 1 > a` is true for `INT_MAX` |
| `more/noret*.cpp` | a `[[noreturn]]` function that returns on one path is a warning; its caller runs into a trap with no report (exit 133) |
| `more/rec.cpp` | recursion three million deep ends with exit 132 and no report |
| `more/sig.cpp` | a signal handler that builds strings, registered with `std::signal`, compiles as safe; the program aborts with no report in both builds (it runs under Clang) |
| `more/attrs.cpp` | `__restrict` on two parameters, `[[gnu::pure]]`, `[[gnu::returns_nonnull]]` on a function that returns `nullptr`, and `#pragma clang loop vectorize(assume_safety)` on a loop with a dependence are all accepted |
| `more/fmt.cpp` | `std::printf(text.c_str())` and `std::printf("%s %d\n", aLong, aString)` are warnings |

And the command line: `weavec-c++ -fweavec-safe -ffast-math
-fstrict-enums -fno-weavec-zero-init` is accepted, each flag a promise
to the optimizer about code the rules then call safe.

### Measured: what it costs to define

`build/rfc51/cost/` builds RFC 0049's G7 `json` port (every function
safe) and times `bench 20000`, median of seven interleaved runs.

| Build | ms | Ratio |
| --- | ---: | ---: |
| `clang++ -O2` | 694 | 1.000 |
| the same with zeroed locals | 704 | 1.015 |
| the same with zeroed locals, `-fwrapv -fno-finite-loops -fno-strict-float-cast-overflow`, and traps for a missing return, unreachable code, division by zero, shifts and invalid `bool`s | 700 | 1.009 |
| the same with signed overflow trapping in place of wrapping | 694 | 1.001 |
| `weavec-c++ -O2 -fweavec-safe` (fast) | 836 | 1.205 |
| the same with `-fwrapv -fno-finite-loops -fno-strict-float-cast-overflow` | 843 | 1.216 |

Everything this RFC defines or stops is inside the noise of one program
under Clang, and the definitions add 1% to the fast build (863 against
876 ms in a second run). One program is not a gate; (D8) and S0 are.

The stops could not be timed under WeaveC, for a reason that is itself a
finding: any `-fsanitize=` flag on the command line of a fast unit
switches off its typed allocations. `configureEnforcement` adds its own
sanitizer kinds only `if (lang.Sanitize.empty())`, and the allocation
types of RFC 0045 are one of them. The `json` port compiled with
`-fsanitize=return` reports `0 typed allocation sites` where it has 15,
and runs at 2.15 times Clang, not 1.2. A project that already builds
with a trapping UBSan check, as Android's and the Linux kernel's
hardened builds do, loses the fast mode without a word. §6 fixes it.

### Measured: natural code at 3,500 lines

The largest new program any gate has measured is the `json` port, 820
lines in one file. For this RFC two agents each wrote a program the way
they would for any C++ project, told only that natural code was wanted
and not to read this repository: a tree-walking interpreter for a small
language (`build/rfc51/trial/interp`, 3,545 lines in 20 files, 37 script
tests) and a persistent key-value server over POSIX sockets with a
thread pool (`trial/kv`, 3,809 lines in 16 sources and 14 headers, 289
checks). Both build and pass with the system compiler.

Unedited, under the rules (`survey.sh`): 79 and 83 distinct errors, 2.2
for every hundred lines, against 7 for existing C++ (RFC 0049's survey).

| | interp | kv |
| --- | ---: | ---: |
| `pointer-cast` | 41 | 3 |
| `lock-held` | | 51 |
| `owner-path` | 18 | 4 |
| `stale-borrow` | 15 | 9 |
| the thread rules' other three | 2 | 3 |
| `call-args`, `call-alias`, `index-single`, `string-index`, `escape`, `owner-moved` | 3 | 13 |

A second agent then made each program safe, with the guide and
`--weavec-explain` and nothing else (`trial/BRIEF.md`; each `record.md`).
`verify.sh` rebuilt both results in both builds and ran their tests.

| | interp | kv |
| --- | --- | --- |
| Agent's time | 6 min | 13 min |
| Rounds (compiles whose errors were read) | 7, clean at the second | 6, clean at the fourth |
| Functions safe | 692 of 692 | 516 of 520 |
| Annotations | 3 `WEAVEC_ALIASED` | 4 `WEAVEC_COUNTED_BY`, 3 `WEAVEC_ESCAPES`, 1 `WEAVEC_SENDS`; 4 three-line socket helpers `WEAVEC_GUARDED` |
| Tests, fast and checked build | 37 of 37 | 289 of 289 |

The loop holds at four times the size any gate had tried, and that is
the first thing this trial says: the rules are not what stands between
an agent and a safe program of this size. Both agents recorded errors
they believed wrong (a `const` global map keyed by `std::string_view`
that is "not safe to share"; a borrow of a token across a call that
only moves an index; a locked class that stops being shareable when it
declares a destructor; a member moved out of an rvalue-reference
parameter ending the whole parameter). They are in the records, were not
verified, and are not this RFC's.

What the same sources contain of this RFC's subject, counted in the
programs' own code by the places where Clang's frontend would emit a
check (`build/rfc51/count.sh`), and by reading:

| In 7,354 lines | interp | kv |
| --- | ---: | ---: |
| Functions that can end without returning their value, as far as the frontend can tell | 1 | 4 |
| `[[noreturn]]` functions | 2 | 2 |
| Signed additions, subtractions and multiplications | 20 | 37 |
| Shifts by a count that is not a constant | 1 | 25 |
| Divisions and remainders by a value that is not a constant | 1 | 7 |
| Conversions from floating point to an integer | 3 | 0 |
| Registrations of a signal handler | 0 | 2 (one handler; and three of `SIG_IGN` or `SIG_DFL`) |
| `std::unreachable`, `[[assume]]`, `__restrict`, `pure` and `const` attributes | 0 | 0 |
| Type names defined in more than one source file | 0 | 0 |

Two things follow. First, the constructs this RFC *rejects* are rare in
what a model writes on its own: one handler in 7,354 lines. The
constructs it *defines* or *stops* are in every file: 150 arithmetic
operations and five functions whose end is reachable. So the loop does
not get harder, and the result changes meaning in a hundred places an
agent never looks at. Second, one agent kept every file's helpers in an
unnamed namespace, ten times in each program, so the `odr/` probe did
not arise. It arises when files are written apart, which is how a large
rewrite is done, and when two existing files are converted that never
shared a header.

### What an agent has in Rust that it does not have here

- **A definition of done that covers what it did not think about.** A
  model writing Rust does not know the list of undefined behavior and
  does not need to: nothing on it is reachable from safe code. A model
  writing safe C++ has the same habits and none of the protection. It
  writes `default:`-less switches because Clang taught it to, and
  `std::unreachable()` because C++23 has it.
- **Stops that say what happened.** Division by zero, `unreachable!()`
  and stack exhaustion each end a Rust program with a message. Here the
  same events end it with exit 132, 133 or 134 and nothing on standard
  error, or do not end it.
- **A program, not a set of files.** `cargo build` compiles one crate
  graph in which a name has one meaning. `weavec-cc` compiles units, and
  the rules of each are decided inside one function. What the units
  assume of each other has never been checked by anything.

### Why this, and why now

Because every other number this project reports is conditional on it.
"692 of 692 functions safe", the audit's first line for a conversion,
the cost of the fast build: each reads as a claim about the program and
is a claim about the rules. The fast mode raised the stakes three times
(RFC 0045, RFC 0048, RFC 0050) by taking the shadow out from behind
safe code on the strength of what safe means, and `odr/` and
`clash.cpp` are programs the checked build stops and the fast build
does not.

And because it is the cheapest of the open problems by a wide margin.
It needs no new annotation, no change to a rule's model, and no decision
about what to believe. Most of it is compiler options that exist, a
pass of a hundred lines, five reports, and one comparison at the link.

It is not the largest lever on the other two columns, and this RFC does
not pretend otherwise. The trial above measured one that is larger
(_Future work_): the interpreter made safe runs at 1.93 times the
interpreter as written, and 1.39 of that is the forms the rules made the
agent write, which no cost gate has measured because every one compared
a safe program with Clang's build of the same safe program. That is the
next RFC's subject. It should be built on a definition of safe that is
finished.

## Soundness

### What is guaranteed that was not

For a safe function of a unit compiled with `-fweavec-safe`, in both
builds:

- **(N1) Arithmetic.** Every integer operation and every conversion
  between arithmetic types has the result §2 states, or the program
  stops (division). No fact the optimizer uses comes from an operation
  having had no overflow.
- **(N2) Control.** Execution leaves a function by a `return` with its
  value, by an exception, or in a call that does not return. Reaching a
  point the source or the language calls unreachable stops the program
  with a report.
- **(N3) Values.** Every object a safe function reads holds a value of
  its type: a local is zeroes until it is assigned, whatever its size,
  and no safe function writes bytes into a `bool` or reads one through
  another member of a union.
- **(N4) Nothing asserted.** No fact the optimizer uses in a safe
  function comes from an attribute, a builtin, a pragma or a flag that
  nothing checks.
- **(N5) The stack.** A safe function's frame lies inside its thread's
  stack, or the program stops with a report before the frame is used.
- **(N6) One definition.** The safe units linked into one image agree
  on the layout and the annotations of every type they share that has a
  name outside its file, on the declaration of every function one calls
  and another defines, and on the type of every global.

Together with what exists: **a program whose every function is safe
either does what its source says, with arithmetic as §2 defines it, or
stops with a report. The one departure is the one (X6) and (Y1) to (Y6)
already state: in the fast build a pointer the rules did not follow may
reach another object of its own type.** That sentence is what the
guarantees page gains, and (D1) is the promise that it is not said
before the list behind it is closed.

### What it rests on

- (N1), (N2) and (N5) rest on code the compiler emits and the runtime's
  handlers. `verify` mode has nothing to monitor: no check is removed.
- (N3) rests on zeroed locals, on two extended rules, and on (T-A1) of
  RFC 0042 for memory that code which is not safe wrote: such code
  stores 0 or 1 in a `bool`.
- (N4) rests on the rule `assumed` (§4.1) and on the driver.
- (N6) rests on the record each safe unit emits and on the link being
  made by `weavec-cc`, which the guarantee already requires.
- The standard library's preconditions stay where RFC 0045 and RFC 0050
  put them: (Y-A4), checked by its hardening where the hardening has a
  check, and the table of §1 says which those are.

### Bugs caught

Each probe of _Motivation_, and in general:

```cpp
enum class Op { Add, Sub };
long step(Op op, long acc, long x) {       // safe; no error today
  switch (op) {
  case Op::Add: return acc + x;
  case Op::Sub: return acc - x;
  }
}                                          // (N2): missing-return, here
```

```cpp
struct Header { unsigned length; bool compressed; };
void load(std::FILE *file, Header &h) {
  std::fread(&h, sizeof h, 1, file);       // (N3): pointer-bytes
}
```

```cpp
// a.cpp                          // b.cpp
struct Entry { long hits; };      struct Entry { long slots[6]; long hits; };
                                  // (N6): definition-mismatch, at the link
```

### Bugs deliberately not caught

- **A stale pointer the rules did not follow**, in the fast build. (Y) is
  unchanged.
- **Overflow as a mistake.** A sum that wraps is defined and wrong.
  Rust's debug build panics there; (D2) argues why the checked build
  does not.
- **Preconditions of the standard library that its hardening does not
  check** and that take no pointer or iterator (which are guarded): a
  `std::mutex` unlocked by a thread that does not hold it, a
  `std::condition_variable` waited on with two mutexes. They are rows of
  the table with the disposition "assumed (Y-A4)", listed, not closed.
- **Code that is not safe.** A `WEAVEC_GUARDED` function may register a
  signal handler, be declared `pure` and write, or define a type its
  neighbors define otherwise. The audit lists what safe code takes from
  it (§8).
- **Units with a `WEAVEC_SAFE` function and no `-fweavec-safe`.** The
  function's own code gets every definition and stop. Library templates
  instantiated into it are compiled with the unit's options, which are
  the command line's (§2.4).

### Accepted rejections and false stops

- **A division by zero whose result was not used** stops. On arm64 it
  ran.
- **A program that reached `std::unreachable()` and went on** stops. It
  was already wrong.
- **Code that reads a struct with a `bool` from bytes** does not
  compile (D6). The fix is a member of type `std::uint8_t`, or reading
  bytes and assigning the members.
- **Code that registers a signal handler in a safe function** does not
  compile (D6). `SIG_IGN` and `SIG_DFL` are accepted.
- **Code with `[[assume]]`, `__restrict` or an asserting attribute** does
  not compile until the token is deleted, which `--weavec-fix` does
  (D4). Nothing else about it changes.
- **Two safe units that define one name two ways** do not link (D7),
  including the ones that happened to work because no inline function
  of the type was shared.
- **A safe unit compiled with `-ffast-math`** is refused by the driver.
- No correct program that compiles and links stops that did not, but for
  the division above.

### Assumptions

- **(N-A1)** The hardware and the system define what the definitions of
  §2 rely on: two's complement, a shift instruction that takes its count
  modulo the width or a mask before it, a guard page below every stack.
- **(N-A2)** A thread's stack has at least one inaccessible page below
  it. A thread that code which is not safe starts with a stack it
  allocated without one has no (N5).
- **(N-A3)** Objects that are not safe units (system libraries, units
  built without `-fweavec-safe`) define the names they share with safe
  units as the safe units' headers declare them. (N6) compares safe
  units with each other.

## Detailed design

### 1. The principle and the list

For a safe function, every behavior the language leaves undefined has
one disposition, and the reference gains a page that is the list
(`docs/pages/reference/undefined.md`, rendered from
`resources/safe/undefined.md` so that `--weavec-explain undefined`
prints it). The dispositions:

| | Meaning |
| --- | --- |
| **rule** | A safe function that contains it does not compile; the row names the rule |
| **defined** | It has one result, which the row states |
| **stops** | The program ends with the report the row names, in both builds |
| **compared** | The link fails when units disagree |
| **library** | A precondition of the standard library: the row says whether its hardening checks it (`library-assertion`), the guards do (it takes a pointer or an iterator), or it is assumed (Y-A4) |
| **stale** | A use of a pointer after its object: the rules, then (Y) |
| **cannot be written** | The construct needs something a rule already rejects; the row names the rule |

The families, with what this RFC changes in bold. S0 expands them to one
row per item of C23 Annex J.2 and of the C++ working draft's annex of
core undefined behavior (or P1705R4's enumeration where the annex has no
entry), and a row that fits no disposition comes back to the owner (D1).

| Family | Today | After |
| --- | --- | --- |
| Signed overflow in `+ - *`, negation, `++`, `--`, `<<` | undefined | **defined**: wraps |
| A shift by a negative count or by the width or more | undefined | **defined**: the count modulo the width |
| Division or remainder by zero; the least value by -1 | undefined | **stops**: `division-by-zero`, `division-overflow` |
| Floating point to an integer that cannot hold it | undefined | **defined**: the nearest value; NaN is 0 |
| Flowing off a function that returns a value | undefined | **rule** where a path provably returns nothing (D5); else **stops**: `missing-return` |
| `std::unreachable()`, `__builtin_unreachable()` | undefined | **stops**: `unreachable` |
| Returning from a `[[noreturn]]` function | a warning | **rule** (D5); a declaration that disagrees with a safe definition is **compared** |
| A loop without side effects that does not end | undefined | **defined**: it does not end |
| A stack that is full; a frame larger than the guard below it | a fault without a report; or no fault | **stops**: `stack-exhaustion` |
| An uninitialized local | zeroes up to 4,096 bytes | **defined**: zeroes |
| A `bool` that is not 0 or 1 | undefined | **rule**: `pointer-bytes`, `union-pointer` |
| `[[assume]]`, `__restrict`, asserting attributes and pragmas | believed | **rule**: `assumed` |
| A `printf` format that is not a literal or does not match | a warning; `%s` and `%n` checked when run | **rule** (D5) |
| A handler run between any two instructions | accepted | **rule**: `signal-handler` |
| Two definitions of one name; a declaration that differs from the definition | undefined, silent in the fast build | **compared**: `definition-mismatch` |
| Out of bounds, null, misaligned, wrong type, cast, pointer made from an integer, variadic arguments, inline assembly, coroutines, `longjmp` | rules and checks of RFC 0041 to RFC 0050 | unchanged; each gets its row |
| Use after release, after move, after scope; iterator invalidation | rules; (Y) in the fast build | unchanged: **stale** |
| A data race | rules of RFC 0046 | unchanged |
| Modifying a `const` object | `pointer-cast` rejects the cast | unchanged |
| The standard library's preconditions | hardening, guards, (Y-A4) | each row says which |

### 2. What is defined (D2)

In a safe function:

1. **Signed `+`, `-`, `*`, unary `-`, `++`, `--` and `<<` wrap** in two's
   complement. `INT_MAX + 1` is `INT_MIN`.
2. **A shift's count is taken modulo the width of the promoted left
   operand.** `1 << 40` on an `int` is `1 << 8`. This is what both
   supported processors do, so it costs nothing where the instruction is
   used and one `and` where the optimizer cannot see that.
3. **A conversion from floating point to an integer type saturates**,
   and NaN converts to 0: Rust's `as`, and what arm64 does natively.
4. **A loop may run forever.** It is not removed for having no effect.
5. **A local is zeroes until assigned, whatever its size** (§2.3).

#### 2.1 Why arithmetic wraps and does not stop

Rust panics on overflow in a debug build and wraps in a release build.
The same split is available here (the checked build could stop), and
this RFC does not take it, for three reasons.

- The rules are the same in both builds and so is what a program
  computes (RFC 0048). An agent that iterates in the checked build and
  ships the fast one should not ship a program whose arithmetic differs.
- A conversion in place meets hash functions, checksums and random
  number generators written with `int`. Each would be a stop in a test,
  with no fix the compiler can write, in code that is already correct
  under `-fwrapv`, which is how the Linux kernel and several of the
  corpus's projects are built.
- A stop for overflow is a check on every addition. Under Clang it cost
  the `json` port nothing measurable; in a loop that sums it is the
  difference between vector code and scalar code.

_Alternatives_ keeps the other answer open as a flag.

#### 2.2 Where it is done

Two places, so that each kind of unit gets what it can.

- **The unit's options**, when the unit is compiled with
  `-fweavec-safe`: `configureEnforcement` sets signed overflow to
  defined (`LangOptions::SOB_Defined`), `CodeGenOptions::FiniteLoops` to
  never, and `StrictFloatCastOverflow` to false. Every function the unit
  emits is then compiled so, including the instantiations of library
  templates, which is what makes `std::accumulate` over `int` defined.
- **A pass over safe functions**, `GuardDefine` at `PipelineStartEP`
  beside `GuardInsert` (`lib/Frontend/GuardDefine.cpp`), for every safe
  function of any unit: it removes `nsw` from `add`, `sub`, `mul` and
  `shl`; masks the count of each `shl`, `lshr` and `ashr` whose count is
  not a constant in range; replaces `fptosi` and `fptoui` with
  `llvm.fptosi.sat` and `llvm.fptoui.sat`; removes `mustprogress` from
  the function and its loops; and removes any `llvm.assume` (none should
  remain after §4.1; the pass does not rely on that). The pass also
  places the stops of §3.

The pass is what makes a `WEAVEC_SAFE` function in a C unit, or in a
C++ unit without the flag, defined in its own code. A unit with no safe
function is not touched by either, and compiles to the object it does
today.

#### 2.3 Locals

RFC 0035 zeroes locals up to 4,096 bytes, because clearing a buffer of
hundreds of KiB on each turn of a loop cost one test suite 14 times. In
a safe function the limit is removed: a local the function reads before
writing is otherwise an undefined value, which the optimizer may choose
differently at each use, and the fast build's frames have no shadow
(RFC 0048). `-fno-weavec-zero-init` is refused for a unit compiled with
`-fweavec-safe` (§6). S0 measures the cost on the conversions; Rust pays
the same for `[0u8; N]`.

#### 2.4 What the pass does not reach

Library code instantiated in a unit that has safe functions and was not
compiled with `-fweavec-safe` keeps the command line's arithmetic. The
guide says to compile safe C++ with the flag, the audit already counts
units, and its plan gains a line for such units (§8).

### 3. What stops (D2, D3)

Each stop is a call to a runtime entry that does not return, placed by
`GuardDefine` in safe functions, reported as every other stop is (the
kind, the place, the stack; JSON under `WEAVEC_RT_REPORT_FORMAT=json`),
and obeying `-fweavec-checks=report` as the guards do.

#### 3.1 Unreachable code and missing returns

Clang lowers `std::unreachable()`, `__builtin_unreachable()` and, with
optimization, the end of a value-returning function to the instruction
`unreachable`; without optimization the last is a bare trap. In a safe
function:

- The end of a function that must return a value becomes a call to
  `__weavec_rt_missing_return(site)`. The driver asks Clang for its
  `return` check (`SanitizerKind::Return`), whose handler,
  `__ubsan_handle_missing_return`, the runtime defines as it defines
  `__ubsan_handle_out_of_bounds` today, so the place is the function's
  closing brace at every optimization level.
- Every other `unreachable` that does not follow a call to a function
  that cannot return becomes a call to `__weavec_rt_unreachable(site)`
  (`SanitizerKind::Unreachable`, handler
  `__ubsan_handle_builtin_unreachable`).

A `switch` then keeps the range check of its table, and the comparison
the library makes before an index is no longer removable by a promise
three lines above it. That is the whole cost: a compare and a branch per
`switch` that had an unreachable default.

A call to a function declared `[[noreturn]]` keeps the attribute. If
the function is safe, (D5) made returning from it an error where it is
defined, and (N6) compares the declaration; if it is not safe, it is
trusted as code that is not safe is.

#### 3.2 Division

Before a `sdiv`, `udiv`, `srem` or `urem` whose divisor is not a
constant other than zero (and, for the signed ones, other than -1), the
pass places a comparison and a call to `__weavec_rt_division(site,
overflow)`. `%` by a constant power of two and `/` by a constant, which
are most divisions in the trial programs' loops, get nothing.

#### 3.3 The stack

- Every safe function whose frame is larger than a page probes it: the
  function attributes `"probe-stack"="inline-asm"` and
  `"stack-probe-size"` set by `GuardDefine`, so that each page of the
  frame is touched in order and the guard page cannot be stepped over.
  A variable-length array and `alloca` in a safe function are already
  in tracked frames (RFC 0041).
- The runtime gives each thread it sees an alternate signal stack
  (`sigaltstack`, at the thread's first allocation or guard, and for the
  main thread at start), and its fault handler runs on it. A fault whose
  address is within a page below the thread's stack is reported as
  `stack-exhaustion` with the stack that was running.

S0 confirms that LLVM's inline probes are emitted for arm64 Darwin, as
they are for arm64 and x86-64 Linux; if not, the pass emits the loop
itself in the function's entry block for frames above a page.

### 4. What is rejected (D4, D5, D6)

#### 4.1 `assumed`: what only asserts

A safe function, and the declaration of a function a safe function
calls when that declaration is in the unit's own files, has none of:

- `[[assume(e)]]`, `__attribute__((assume(e)))`, `__builtin_assume(e)`,
  `__builtin_assume_aligned`, `__builtin_assume_separate_storage`;
- `__restrict`, `__restrict__` and C's `restrict` on a parameter, a
  local or a member;
- the attributes `pure`, `const`, `malloc`, `alloc_size`, `alloc_align`,
  `assume_aligned`, `returns_nonnull` and `nonnull`;
- `#pragma clang loop vectorize(assume_safety)`, `#pragma GCC ivdep`,
  `#pragma omp simd` and `#pragma clang loop` with `interleave` or
  `distribute` in their asserting forms.

The fix is certain (RFC 0049's sense) and is the deletion of the token,
the attribute or the line; `--weavec-fix` writes it. For `[[assume(e)]]`
the note adds that `if (!(e)) std::unreachable();` states the same and
is checked, since (D3) makes it a stop.

`[[likely]]`, `__builtin_expect`, `[[nodiscard]]`, `noexcept`,
`[[clang::lifetimebound]]` and alignment specifiers assert nothing the
optimizer may act on without a check, and are accepted. `[[noreturn]]`
is handled by §3.1 and (D5).

#### 4.2 Bytes and `bool`

`pointer-bytes` rejects reading or writing as bytes an object that holds
pointers, and in C++ one that is not trivially copyable. It now also
rejects **writing bytes into an object that has a `bool`** at any depth:
the destination of `memcpy`, `memmove`, `fread`, `read`, `recv`, their
kin in the table (every row whose argument is `w:bytes(...)`), and
`std::bit_cast` to a type with a `bool`. Zeroing a whole object and
copying a whole object of the same type stay accepted, as they are for
pointers. `union-pointer` rejects a union in which a `bool` shares bytes
with a member of another type.

An enumeration needs no such rule: Clang assumes nothing of an
`enum`'s value unless `-fstrict-enums` says so, and §6 refuses that
flag.

#### 4.3 `signal-handler`

The table gains the trait `handler` for a function that registers code
the system runs between any two instructions of the program: `signal`,
`sigaction`, `bsd_signal`, `sigset`, `sigvec`. A safe function calls
`signal` only with `SIG_IGN` or `SIG_DFL` and does not call the others.
The note says what the kv trial's author did unprompted: register the
handler in a `WEAVEC_GUARDED` function, let it store to a `std::atomic`
or write a byte to a pipe, and do the work on a thread; or block the
signal and wait for it with `sigwait`. The audit lists every function
that is not safe and calls a `handler` row (§8).

A rule that accepted a handler by checking its body (only atomics and
functions the table calls safe in a handler) would be the better
answer, and is _Future work_: it needs a property of functions that
calls carry, which no rule has yet.

#### 4.4 Clang's diagnostics (D5)

In a unit compiled with `-fweavec-safe`, four groups of Clang's
warnings are errors: `-Wreturn-type` (a path that returns no value),
`-Winvalid-noreturn`, `-Wformat` and `-Wformat-security`. In C the
groups Clang already makes errors by default (an implicit function
declaration, an integer converted to a pointer, a pointer to another
type) cannot be turned back into warnings, and
`-Wdeprecated-non-prototype` joins them: a call through a declaration
without a prototype passes arguments nothing compared.

They keep Clang's text and Clang's tag (`[-Werror,-Wreturn-type]`). The
argument is RFC 0049's own: "A model has read a great deal of Rust and
none of WeaveC's dialect." It has read `non-void function does not
return a value in all control paths` ten thousand times. Rewording it to
carry a `[weavec-safe:...]` tag would spend the one prior a model has
here. The rules' reference gets one page, `defined`, that lists the four
and why each is an error in safe code, and `test/Safe/defined.cpp` pins
their text. Rule 2 of `AGENTS.md` is amended to say so.

A `WEAVEC_SAFE` function in a unit without the flag gets the stops of §3
for the first two and the run-time checks of RFC 0037 for formats, not
the errors. S0 decides whether to promote them for such functions from
the checker instead.

#### 4.5 Flags

`unsupportedConfiguration` in `Driver.cpp` refuses, for a compile with
`-fweavec-safe`: `-ffast-math`, `-Ofast`, `-ffinite-math-only`,
`-fstrict-enums`, `-fstrict-vtable-pointers`, `-ffinite-loops`,
`-fstrict-overflow`, `-fno-wrapv`, `-fstrict-float-cast-overflow` and
`-fno-weavec-zero-init`, each with one line: `weavec-cc: error:
'-ffast-math' lets the optimizer assume what a safe unit does not check
(RFC 0051)`. `-ffp-contract`, `-fno-math-errno` and `-fno-trapping-math`
change results and assume nothing, and are accepted.

### 5. One definition (D7)

#### 5.1 The record

A unit with safe functions emits one section, `__DATA,__weavec_defs`
(`.weavec.defs` on ELF), kept through the link of an image so that an
image linked against a shared library `weavec-cc` built is compared with
it. It holds a version and three lists, each entry a name, a 64-bit
hash and a place (file, line, column):

- **Types.** Every class, struct, union and enumeration that is defined
  outside system headers, has a name other units can say (not in an
  unnamed namespace, not local to a function) and is complete in the
  unit. The name is the mangled type name. The hash covers Clang's ODR
  hash of the definition (`CXXRecordDecl::getODRHash`, `EnumDecl`'s, and
  `RecordDecl`'s in C), the layout (size, alignment, each base's and
  each field's offset, the order of the virtual functions) and the
  WeaveC annotations of the class and of each member (bounds, owned,
  borrowed, aliased, guarded).
- **Functions.** Every function with external linkage that the unit
  defines or that a safe function of the unit calls, declared outside
  system headers. The name is the linker's. The hash covers the return
  type, the parameter types, `noreturn`, `noexcept`, whether it is
  variadic, and every WeaveC annotation of the declaration.
- **Globals.** Every variable with external linkage the unit defines or
  a safe function names. The hash covers its type with its array bound,
  `const`, `thread_local` and its annotations.

The bodies of inline functions are not compared. Each was checked by the
rules in its own unit against types that (N6) has shown equal, so
whichever copy the linker keeps is a safe function of the same types.

#### 5.2 The comparison

When `weavec-cc` runs a link, before the linker runs, it reads the
section from every object, every member of every archive, and every
shared library on the line that has one (`llvm::object`, in the process;
`lib/Frontend/Definitions.cpp`), and compares entries of the same kind
and name. A difference is an error and the linker is not run:

```text
weavec-cc: error: 'Entry' is defined differently in two safe units [weavec-safe:definition-mismatch]
cache.cpp:5:8: note: here it is 8 bytes: 'long hits'
index.cpp:7:8: note: here it is 56 bytes: 'std::array<long, 6> slots', 'long hits'
weavec-cc: note: a program has one definition of each name: put a type only one file uses in an unnamed namespace, or define it in one header both include
```

The two notes say what differs when the driver can tell from the
record (the record keeps a short description beside the hash: the size
and the members' spellings for a type, the declaration's text for a
function or a global), and otherwise only where.

Objects without the section are not compared: code that is not safe is
checked by the shadow where it meets safe code (the validating entries
of RFC 0041), and (N-A3) covers the rest. `-fweavec-summary` on a link
adds a line: `weavec: <output>: 31 objects, 29 safe; 412 types, 1,306
functions and 9 globals agree`.

#### 5.3 What this is not

It is not whole-program analysis. Nothing is inferred from another
unit; a unit's rules are decided as before. It is the comparison a
module system makes when it imports an interface, done at the one
moment C and C++ put the pieces together.

### 6. The driver

- `configureEnforcement` sets the options of §2.2 and the sanitizer
  kinds of §3.1 for a unit compiled with `-fweavec-safe`, and
  `TrivialAutoVarInitMaxSize` to none for it.
- **A fix.** WeaveC's own kinds (`ArrayBounds`, `AllocToken`, and now
  `Return` and `Unreachable`) are set whatever the command line's
  `-fsanitize=` says, and the command line's kinds keep their own
  handlers beside them. A fast unit compiled with a UBSan check keeps
  its typed allocations. `test/Driver/safe-checks-modes.cpp` pins it.
- `unsupportedConfiguration` gains §4.5.
- The link step gains §5.2. `-fno-weavec-definitions` skips the
  comparison and says so on standard error; it exists for bisecting a
  build, and the audit reports a ledger set whose link used it.

### 7. The runtime

- `__ubsan_handle_missing_return`, `__ubsan_handle_builtin_unreachable`
  (and their `_abort` forms where Clang emits them), and
  `__weavec_rt_division`, each a trivial entry over a non-inlined helper
  (rule 9 of `AGENTS.md`), declared in `weavec_rt.h` and listed in
  `weavec_rt.exports`.
- The alternate signal stack and the `stack-exhaustion` report (§3.3),
  in `weavec_report.c`.
- Five report kinds (_Diagnostics_). `WEAVEC_RT_ABI` becomes 47.

### 8. The ledger, the audit and the summary

- **Ledger 13.** Each unit records `"defined"`: whether it was compiled
  with `-fweavec-safe` (so that its library instantiations are defined
  too), and the counts of what `GuardDefine` placed: stops by kind,
  shifts masked, conversions made saturating. The `errors` list of RFC
  0049 carries `assumed`, `signal-handler` and the new cases of
  `pointer-bytes` with their fixes.
- **Audit 9.** A new first paragraph, "undefined behavior": the units
  with safe functions that were not compiled with `-fweavec-safe`; the
  functions that are not safe and register a handler, carry an asserting
  attribute safe code can see, or were compiled with a refused flag;
  whether the ledgers' link was compared. When all three lists are empty
  and every function is safe it prints `nothing undefined: every
  function is safe and the units agree`.

### 9. Safe C

Everything above holds for Safe C: the pass runs on every safe function,
and C has no library templates for §2.4 to miss. The rows of the list
that are C's alone get their disposition in S0; the ones known now:
a call through a declaration without a prototype (rejected, §4.4), two
declarations of one function with different types (compared, where C++
would fail to link), and a function that ends without a value whose
caller uses one (stops, as in C++, where C leaves it to the caller).

### 10. What is written down

- `docs/pages/reference/undefined.md` (from `resources/safe/undefined.md`):
  the list, one row per item, with its test.
- `docs/pages/reference/guarantees.md`: (N1) to (N6), (N-A1) to (N-A3),
  and the sentence of _Soundness_ at the top of "Safe functions".
- The guides: "What safe code can rely on" in `safe-cxx.md` and
  `safe-c.md` (arithmetic, stops), a row of "Coming from Rust" for each
  of overflow, `unreachable!()`, `bool` and modules, and the signal
  handler as a building block.
- `resources/safe/rules.md`: `assumed`, `signal-handler`,
  `definition-mismatch`, `defined`, and the new parts of `pointer-bytes`
  and `union-pointer`, each with a rejected and an accepted example that
  `check-examples.py` compiles.
- `docs/pages/reference/cli.md`: the five kinds, the refused flags,
  `-fno-weavec-definitions`, the link's summary line.
- `AGENTS.md`: rule 2, and rows for `GuardDefine.cpp` and
  `Definitions.cpp`.

### 11. Cleanups and breaking changes

- **Breaking:** in safe functions, signed overflow wraps, shifts and
  float conversions have one result, and division by zero stops.
- **Breaking:** `std::unreachable()`, `__builtin_unreachable()` and a
  missing return stop.
- **Breaking:** `assumed`, `signal-handler`, `bool` in `pointer-bytes`
  and `union-pointer`; four Clang warnings as errors; ten flags refused.
- **Breaking:** safe units that disagree do not link.
- **Removed:** the 4,096-byte limit for safe functions;
  `-fno-weavec-zero-init` for safe units; the `Sanitize.empty()` test in
  `configureEnforcement`; the bare traps a `[[noreturn]]` function's
  caller and a full stack end in.
- **Not changed:** a unit with no safe function (it compiles to the same
  object but for the ABI number); the rules of bounds, ownership,
  borrows and threads; the fast and the checked modes' division of
  labor; `weavec.h`.

### 12. Stages and gates

- **S0 Measure and list (D1, D8).** Nothing below is kept until these
  are written down in `build/rfc51/`.
  (a) The list: one row per item of the two standards' enumerations,
  each with a probe that reaches it from a safe function or the rule
  that keeps it from being written. The rows whose disposition is not
  one of §1's.
  (b) The cost of §2 and §3, each part switched on alone by a throwaway
  option: the two `json` ports, the LRU port, tinyxml2, yaml-cpp, the
  two trial programs, Safe C's five. The cost of zeroing every local.
  (c) What §4 rejects in code that exists: the trial programs, RFC
  0044's 90 correct files, RFC 0046's 40, the conversions of RFC 0047
  to RFC 0049, libzmq's and nng's safe parts.
  (d) §5.1's record over libzmq: its size, the time of the comparison,
  and every mismatch it finds among units that link today, each read.
  (e) The probes this RFC could not finish: a null pointer used at an
  offset larger than the unmapped pages (a reference bound through a
  null pointer to an object of more than 64 KiB), a `std::unique_ptr`
  to a base without a virtual destructor (`more/dtor.cpp`: one case
  stops with `invalid-free`, one skips the derived destructor),
  `std::bit_cast` to a pointer (rejected today with `escape`'s words).
  **The decision:** a part that costs any program more than 3% is
  reported with its number before it is built further (D8).
- **S1** `GuardDefine`: §2. **S2** The stops and the runtime: §3, §7.
  **S3** The rules, Clang's diagnostics and the flags: §4, §6. **S4**
  The record and the comparison: §5. **S5** The ledger and the audit:
  §8. **S6** The pages: §10. **S7** The gates.

Gates:

- **G1 Tests.** `test/Guards/ir-defined.cpp` and `ir-defined.c` (no
  `nsw`, masked shifts, saturating conversions, no `mustprogress`, the
  stops' calls, a probed frame; none of it in a guarded function of the
  same unit), `test/Guards/unreachable.cpp`, `missing-return.cpp`,
  `division.c`, `stack-exhaustion.cpp` (each kind's report line),
  `test/Safe/assumed.cpp` and `assumed.c`, `signal-handler.cpp`,
  `bool-bytes.cpp`, `defined.cpp`, `fix-assumed.cpp`,
  `test/Driver/safe-flags.cpp`, `definitions.cpp` (two units and a
  header: agreeing, a type, a function, a global, an archive, a shared
  library, the summary line), one unit test per `handler` row,
  `unittests/Frontend/DefinitionsTest.cpp`.
- **G2 The list is closed (D1).** Every row of
  `resources/safe/undefined.md` names a disposition of §1 and a test
  that exists; a script checks both; no row says "undefined".
- **G3 The probes.** Every program of `build/rfc51/probe` fails to
  compile, fails to link, or stops with a report, in the fast and in the
  checked build. None exits 0 after doing what _Motivation_ records.
- **G4 Cost (D8).** Fast, against main's fast build of the same source,
  median of seven interleaved: both `json` ports, the LRU port,
  tinyxml2, yaml-cpp, the two trial programs' benchmarks and Safe C's
  five, each at most 1.03. A compile at most 1.05 times main's; the
  comparison at most 2% of libzmq's link.
- **G5 Natural code.** The two trial programs as written: the errors
  this RFC adds, counted (expected: the handler's registration in kv).
  RFC 0044's and RFC 0046's correct files: every file newly rejected,
  read. The conversions: every new error has a certain fix, and
  `--weavec-fix` alone clears the `assumed` ones.
- **G6 No false stop.** The tests of both ports, both trial programs
  and every conversion pass in both builds with no report of a new
  kind; a report of a new kind in them is read and is a defect of the
  program, or the gate fails.
- **G7 The loop is not worse.** The `json` port written again by a
  fresh agent: rounds and time beside RFC 0050's (2 rounds, 5 min 17 s).
  Each trial program made safe again by a fresh agent: rounds and time
  beside this RFC's.
- **G8 Nothing elsewhere.** A unit with no safe function compiles to
  main's object but for the ABI number. The `dev` tests, the cases, the
  quick corpus gate and the documentation's examples pass; the Linux
  container's `ci-release` passes.
- **H1 Hygiene.** The code lines this RFC adds to and removes from the
  library and the runtime, reported (RFC 0050 amendment 12).

## Annotation surface

None. `resources/include/weavec.h` stays at version 3.4.

## Diagnostics

**Rule errors**, each with a page and pinned in `test/Safe/`:

```text
error: '<what>' tells the optimizer what nothing checks [weavec-safe:assumed]
note: fix (certain): remove '<what>'
error: bytes are written into '<object>', which holds a 'bool' ('<member>') [weavec-safe:pointer-bytes]
note: a 'bool' is 0 or 1: give the member the type 'std::uint8_t', or read the bytes and assign each member
error: '<member>' is a 'bool' that shares its bytes with '<other>' [weavec-safe:union-pointer]
error: '<function>' registers a handler that runs between any two instructions of the program [weavec-safe:signal-handler]
note: register it in a WEAVEC_GUARDED function and let it only set a 'std::atomic' or write to a pipe; or block the signal and wait for it with 'sigwait' on a thread
```

**The link's error** (§5.2), pinned in `test/Driver/definitions.cpp`:
`[weavec-safe:definition-mismatch]`.

**Clang's own**, errors in a safe unit under their names (§4.4), pinned
in `test/Safe/defined.cpp`.

**The driver's**, pinned in `test/Driver/safe-flags.cpp`:

```text
weavec-cc: error: '<flag>' lets the optimizer assume what a safe unit does not check (RFC 0051)
```

**Report kinds**, each pinned in `test/Guards/`, listed in the
command-line reference and known to `scripts/run-cases.py`:

```text
weavec: unreachable at <file>:<line>:<col>: reached code the program says cannot be reached
weavec: missing-return at <file>:<line>:<col>: '<function>' ended without returning a value
weavec: division-by-zero at <file>:<line>:<col>: division by zero
weavec: division-overflow at <file>:<line>:<col>: the least '<type>' divided by -1
weavec: stack-exhaustion at <file>:<line>:<col>: thread T<n>'s stack of <size> is full
```

Minimal programs: `ub8.cpp` (`unreachable`), `ub7.cpp`
(`missing-return`), `more/arith.cpp` (both divisions), `more/rec.cpp`
and `more/clash.cpp` (`stack-exhaustion`).

## Drawbacks

- **It makes nothing faster and no conversion shorter.** The two
  columns the last four RFCs worked on do not move, and (D8) allows one
  of them to move the wrong way by 3%.
- **The list is long and someone must keep it.** A new version of the
  language adds undefined behavior, and a new version of Clang adds
  attributes that assert. The page of §10 is a thing to maintain, and G2
  only checks that each row has a test, not that no row is missing.
- **The optimizer loses facts it used.** Signed induction variables
  widen less freely without `nsw`; a `switch` keeps a range check; a
  pure function is called twice. Measured at 1% on one program.
- **A comparison at the link is new machinery in a new place**: reading
  objects and archives in the driver, a section format with a version,
  and the first error that names two files. It is also the first thing
  in the compiler that is not local to a unit, and the argument that it
  is only a comparison has to keep being true.
- **Clang's diagnostics as errors break the one-tag rule**, and their
  wording changes with Clang.
- **`signal-handler` rejects correct code** (the kv trial's handler is
  safe in fact) and sends it to `WEAVEC_GUARDED`, which is the wrong
  direction for a rule to push.
- **Existing safe code breaks in four ways at once**, in this project's
  own conversions among others.

## Alternatives

- **Do nothing and say so**: keep the guarantees as properties of
  pointers and list undefined behavior under "What is not caught". It is
  honest and it concedes the comparison this project exists to make.
- **Stop on overflow in the checked build**, as Rust's debug build
  does. §2.1 argues against it. It can be a flag
  (`-fweavec-safe-checks=full,overflow`) later without changing anything
  here.
- **Turn on all of UBSan in trap mode.** It is the same mechanism for
  the stops and it is what this RFC measured. It does not define
  anything (overflow stops), it instruments guarded functions and
  library code alike, it knows nothing of assertions, bytes, handlers or
  the link, and its checks for alignment, null and object size repeat
  what the rules and the guards already do.
- **Ignore assertions instead of rejecting them.** The standard lets an
  implementation ignore `[[assume]]`, and the pass removes `llvm.assume`
  anyway. But `__restrict` and `pure` are attributes of declarations
  that other compilers and other units act on, and a safe source that
  means something else under Clang is not "ordinary C++ that compiles
  with any compiler" (the guide's promise). An error with a certain fix
  costs one run of `--weavec-fix`.
- **Compare definitions when the program starts**, from the records
  each image registers. No driver work, and it would cover `dlopen`. It
  moves an error an agent should see when it builds to a stop when it
  runs, and adds to the runtime's start, which RFC 0050 measured at 3 ms
  and counted as a cost.
- **Put a hash in symbol names** (an ABI tag on every project class, or
  on the safe entry of each function). The linker then does the
  comparison and reports it as an undefined symbol. It changes the names
  a debugger and every report show, breaks linking with code that is not
  compiled by `weavec-cc`, and cannot say which two places disagree.
- **C++ modules.** They are the language's answer to (N6), and almost
  no code an agent is asked to convert, and little it writes, uses them.
- **Work on cost instead**: checked casts in place of `dynamic_cast`,
  and sum types in aliased classes (_Future work_). Larger for the
  program's speed, measured for the first time by this RFC's trial, and
  better built on a finished definition of safe than beside an
  unfinished one.

## Prior art

- **Rust.** The reference's "Behavior considered undefined" is a short
  list, and the language's promise is that none of it is reachable from
  safe code; its "not considered unsafe" list (overflow, leaks,
  deadlocks) is this RFC's "defined" and "not caught". RFC 560 chose
  wrapping in release builds and panics in debug builds for integer
  overflow; `as` casts from floats saturate since 1.45; stack probes are
  required on every tier-1 target because safe code can declare a large
  array.
- **"A Proposal for a Friendly Dialect of C"** (Cuoq, Flatt, Regehr,
  2014) lists the same definitions (wrapping, bounded shifts, no
  reasoning from unreachable code), and Regehr's "The Problem with
  Friendly C" (2015) records why agreeing on one such dialect for all of
  C failed: every user wanted a different list. This RFC needs agreement
  only for functions that are already in a dialect.
- **The C++ committee's own direction.** P1705 enumerated core
  undefined behavior, and the working draft now carries the list as an
  annex; P2795 made reads of uninitialized variables erroneous rather
  than undefined in C++26; P2809 defined trivial infinite loops; P3100
  proposes that each core undefined behavior become a contract
  violation that can be checked. Profiles (P3081) group the same
  families as "type", "bounds", "lifetime" and "arithmetic". G2's list
  is P1705's, with a disposition per row.
- **Hardened builds in production.** The Linux kernel compiles with
  `-fno-strict-overflow`, `-fno-delete-null-pointer-checks` and zeroed
  locals, and offers UBSan's checks as traps; Android enables the
  integer and bounds sanitizers in trap mode for media code; Chromium
  ships libc++'s hardening and zeroed locals. Each reports costs of the
  order this RFC measured.
- **Zig's `ReleaseSafe`**: `unreachable` and overflow are checked
  illegal behavior, and the same source is unchecked in `ReleaseFast`.
  (D3) keeps one meaning in both builds.
- **Clang's ODR hash**, computed for modules to diagnose two definitions
  of one class, and reused by §5.1; GCC's `-Wodr` under LTO, gold's
  `--detect-odr-violations` and AddressSanitizer's check of globals with
  one name and two sizes, each of which finds part of (N6) late or
  under a mode nobody ships.
- **Fil-C** makes all of C's undefined behavior a defined stop with
  capabilities and a collector, at a cost of 1.5 to 4 times. It shows
  the sentence of _Soundness_ can be said for C; this project says it
  for the safe subset at a few percent.
- **Swift 6.2's strict memory safety** rejects `unsafe` constructs by
  default and imports C and C++ through annotated interfaces; its
  integer arithmetic traps, and its modules give it (N6).

## Unresolved questions

1. **Is the list closed?** S0 (a). The families of §1 are written from
   knowledge of the two standards, not from their text. A row that
   needs a rule this RFC does not have comes back to the owner.
2. **Does defining signed arithmetic cost a loop its vector code**
   anywhere in the corpus? S0 (b), per program, with the loop named.
3. **Zeroing every local.** RFC 0035's limit exists because of a
   measured 14 times. Does a safe function in any conversion declare a
   large buffer in a loop? If so: a compile error for a local above a
   size that is read before it is written, where the checker can tell,
   or the cost.
4. **Inline probes on arm64 Darwin** (§3.3), and whether the main
   thread's guard is where the runtime thinks it is.
5. **How many mismatches does (N6) find in code that links and passes
   its tests?** S0 (d). If libzmq has dozens that are harmless (a type
   declared twice with members in `#ifdef`s that no shared function
   touches), the rule is right and the first run is painful; the fixer
   cannot write these.
6. **What a template's record is.** A specialization is compared by its
   mangled name. Is a class template's own definition compared when two
   units instantiate it differently, or only the instantiations?
7. **Shared libraries without the section** that define a type a safe
   unit also defines. (N-A3) assumes they agree. Should the validating
   entries' size check be the stated backstop?
8. **Should `signal-handler` wait** for the rule that checks a
   handler's body, so that it never pushes correct code to
   `WEAVEC_GUARDED`?
9. **A `WEAVEC_SAFE` function in a unit without the flag** (§2.4,
   §4.4). Should such a unit be an error in C++, now that safe units are
   the documented way and a mixed unit has a weaker (N)?
10. **Division.** Is one stop for both cases enough, and should
    `INT_MIN / -1` wrap instead, as multiplication does?
11. **The null row** (S0 (e)): whether a reference bound through a null
    pointer needs a check where it is bound, for objects larger than the
    unmapped low pages.
12. **The base-destructor row** (S0 (e)): a `std::unique_ptr<Base>`
    made from a derived object when `Base` has no virtual destructor is
    undefined; it stops or leaks today. A rule in `SafeCxx.cpp` is a few
    lines; is it this RFC's?

## Future work

- **The cost of the forms the rules ask for.** The trial's interpreter,
  as written, runs its benchmark in 1,044 ms under Clang; made safe, in
  1,455 ms under Clang and 2,019 ms in the fast build: 1.93 times the
  program as written, where the gates of RFC 0045 to RFC 0050 would
  have recorded 1.39. Of the rewrite's 1.39, the 41 `dynamic_cast`s that
  replaced `static_cast`s after a test of the node's `kind` are 1.28 on
  their own, and the `Value` that could no longer be a `std::variant`
  inside an aliased class (32 bytes to 104) is 1.13
  (`trial/bench-interp.sh`; medians of five). The key-value server's
  rewrite cost nothing (1.80 and 1.84 million operations a second under
  Clang, best of three, in a benchmark that writes a log) and its fast
  build runs 1.27 million, 1.45 times the time. Two designs follow,
  and each should be its own RFC: **checked casts** (a `static_cast`
  down a hierarchy, and a conversion from `void *`, compiled as one
  comparison with the object's type, which the pools already keep, in
  place of an error; it is also what nng's 632 `void *` adapters and the
  41 errors above ask for), and **sum types in shared objects**.
- **A cost gate against the program as written**, not against Clang's
  build of the safe program, for every port and trial from now on.
- **A handler rule that checks the handler** (§4.3).
- **`-fweavec-safe-checks=full,overflow`** (§2.1).
- **A gate at the link for the whole program**: refuse to link an
  executable that has a function that is not safe and is not on a list
  the project keeps, as `#![forbid(unsafe_code)]` does for a crate. §5
  reads what it needs.
- **The comparison across `dlopen`**, from the same records, when a
  library is loaded.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what the gates found. They override the body.

1. **The list (§1, D1).** `resources/safe/undefined.md` has a row for each
   of the 221 items of C23's Annex J.2 (by the item numbers of the working
   draft N3220) and each of the 80 entries of the C++ working draft's
   annex of core undefined behavior (by their labels), in our own words;
   `scripts/check-undefined.py` (CTest `undefined-list`) checks that every
   row names a disposition and a test that exists, and that none is
   missing. Two dispositions were added to §1's: **translation** (Clang
   rejects the item, or gives it one meaning, when it translates the unit,
   and the linker when it links: lexing, preprocessing, constant
   expressions, declarations Clang refuses; `test/Safe/undefined-translation.c`
   and `.cpp` pin a sample) and **assumed** (the item is in code that is
   not safe, which T-A1 assumes keeps the guarantees). No row says
   "undefined". The library's preconditions that nothing checks are
   **library** rows that say "assumed of the library (A2)", as §1 allowed.
2. **The stops (§3.1), as built.** C++'s missing return is Clang's
   `SanitizerKind::Return` in a unit compiled with `-fweavec-safe`, as
   designed; `SanitizerKind::Unreachable` is not used. `GuardDefine`
   places every other stop itself, in the safe functions of any unit: an
   `unreachable` the source reaches becomes `__weavec_rt_unreachable`, and
   so does a call of a function whose code is only `unreachable` (libc++'s
   `std::unreachable()`, which inlines to it); one after a call that does
   not return stays. In C the end of a function that must return a value
   is found by the `retval` slot only `return`s write: a flag beside each
   write, which the optimizer folds away where every path returns
   (`__weavec_rt_missing_return`). In a C++ unit without the flag a
   `WEAVEC_SAFE` function's missing return stops as `unreachable` with
   optimization, and as Clang's own trap without a report without it.
   libstdc++'s `std::unreachable()` under its assertions traps by itself
   before the stop (a stop without WeaveC's report); the tests use
   `__builtin_unreachable()` there.
3. **The reports (Diagnostics), as built.** `missing-return` says "the
   function ended without returning a value" (the stack names the
   function: Clang's check carries only a place); `division-overflow`
   says "the least <n>-bit integer divided by -1"; `stack-exhaustion`'s
   place is the first frame with a line (a probe faults in a prologue).
   Every stop ends the program in every mode, `report` included: there is
   no value to go on with. `-fweavec-checks=report` does not make them
   print once and continue.
4. **More defined (§2).** A safe function's `llvm.memcpy` (an assignment of
   a struct, which Clang copies so) is an `llvm.memmove`: an object
   assigned to one it overlaps is copied as it was (J.2 item 51,
   `expr.assign.overlap`; a probe showed it was not caught). `llvm.abs`,
   `llvm.ctlz` and `llvm.cttz` have a result for the least value and for
   zero. In a unit compiled without `-fweavec-safe`, a safe function's
   locals above Clang's 4,096 bytes are zeroed where each scope starts.
5. **More rejected (§4).** Found while giving the C++ list its
   dispositions: `pointer-cast` rejects a member pointer converted to a
   base class's (`ub:expr.mptr.oper.not.contain.member`), and an owner of
   a base made from an owner of a derived object when the base's
   destructor is not virtual (`std::unique_ptr<Shape>` from
   `std::make_unique<Circle>()`; `ub:expr.delete.dynamic.type.differ`; a
   class the table calls `shared` keeps its deleter and is accepted).
   `assumed` covers `static` in a C array parameter (`int a[static 4]`,
   which Clang passes on as `dereferenceable`, J.2 item 74) without a fix.
   A writable byte view (C's `char *`, C++'s non-`const` byte cast) of an
   object holding a `bool` is `pointer-bytes`, as a library call writing
   one is. `-Wunsequenced` joins the four warnings that are errors (J.2
   item 34); the list is five, and six in C.
6. **Less rejected (§4.1).** The allocation attributes (`malloc`,
   `alloc_size`, `alloc_align`) on a function that is not safe and that a
   safe function calls are accepted: a project's own allocator states
   them, Safe C types an allocation by them (RFC 0045), and the runtime
   tracks the block whatever they say (`test/Safe/ownership.c`'s
   allocators). On a safe function's own declaration they are rejected.
   C++ drops `__restrict` from a parameter's type (no `noalias` is
   emitted), so there is nothing to reject there; Clang applies `restrict`
   to nothing but parameters, so a member's is not rejected either.
7. **The fixes (§4.1).** RFC 0049's fixes only inserted; a fix now may take
   text out (`FixEdit::removed`, the ledger's `removed`, the fixer's
   `Inserted::removed`, put back when a fix is withdrawn; the note's fix-it
   is a removal). An attribute alone in its brackets goes with them, and an
   `[[assume]]` on an empty statement with its `;`;
   `__builtin_assume_aligned(p, n)` becomes `p`, and `assume_safety`
   becomes `enable`. The fix's aspect is `removal`.
8. **The thread stacks (§3.3, §7).** A thread gets its alternate signal
   stack (256 KiB, marked so that only the runtime's is given back) before
   its start routine runs: on Darwin from the threads' introspection hook,
   on Linux from the runtime's `pthread_create`, which wraps the start
   routine (weak, so that a `-static` program's C library keeps its own;
   such a program's threads get theirs with their first allocation). The
   main thread's is made in the fault handler's constructor. A fault
   within 256 KiB below the thread's stack and one page into it is
   `stack-exhaustion`. LLVM's inline probes (`"probe-stack"="inline-asm"`)
   work on arm64 Darwin, for fixed frames and variable-length arrays alike.
9. **The record (§5.1), as built.** Function entries hash the result's and
   each parameter's type as the function's type has them, not the whole
   type: a defaulted member's exception specification is computed when a
   unit needs it, so two units printed one declaration two ways, and a
   definition's `T *const` parameter is its own (libzmq's links failed on
   both until this). Defaulted and implicit functions are not recorded
   (the class's entry covers them), nor template patterns and
   specializations (unresolved question 6: only the classes that are not
   templates are compared), nor annotations the checker adds (a fast
   unit's field annotations, placed locals) or that say whether code is
   safe. A global's entry has its alignment when it has one (J.2 item 70);
   an array of unknown bound is not recorded. In C a tag is compared only
   between entries that share a user, as designed.
10. **The comparison (§5.2), as built.** It reads the link's file inputs
    (objects, every member of an archive), each once and in parallel, in
    the read that also finds the program's own allocator (RFC 0032 §2.6);
    a library named by `-l` is not read, and a linked image has no record
    (amendment 16). It runs only for an enforcing link.
    The summary line is printed only with `-fweavec-summary`. The audit
    does not say whether a link was compared (a ledger is a unit's).
11. **The audit (§8), as built.** Version 9 lists the units with safe
    functions not compiled with `-fweavec-safe` and the functions that
    are not safe and register a handler (the ledger's new `handlers`
    count of each function); "the asserting attributes safe code can see"
    is not a list, since §4.1 rejects them. With every function safe and
    none listed it prints `nothing undefined: every function is safe, and
    every unit with safe functions was compiled with -fweavec-safe`.
12. **The driver (§6).** WeaveC's sanitizer kinds are added whatever the
    command line asks (`test/Driver/safe-sanitizers.cpp`), as designed.
    The refused flags are refused for a compile with `-fweavec-safe`
    unless a later flag takes them back (`-ffast-math -fno-fast-math`,
    `-Ofast -O2`). `-fno-weavec-definitions` is a link flag the compiles
    do not see.
13. **RFC 0044's claim.** `test/Driver/safe-cxx-units.cpp` said a checked
    safe C++ unit compiles to the object the unit gives without the flag.
    With RFC 0051 it does not (its arithmetic is defined, its stops placed,
    its definitions recorded); the test says so.
14. **Not built.** `unittests/Frontend/DefinitionsTest.cpp`: the record and
    the comparison are tested through the driver
    (`test/Driver/definitions.cpp`: a type, a global through an archive, C's
    shared and private tags, agreement and its summary line,
    `-fno-weavec-definitions`).
15. **The comparison's gate (2026-11-20).** The owner kept G4's bound on
    the comparison at the link at 2% of libzmq's link: the RFC stays
    Accepted until a change meets it. Amendment 16's change meets it
    (_Measured gates_).
16. **The record is not kept through a link (§5.1).** §5.1 kept the
    record in `__DATA` so that an image could be compared with a shared
    library it links against; kept, the records were half of libzmq's
    shared library (2.87 of 5.67 MB), loaded with it. The section is
    instead one a linker does not copy into what it links: on Mach-O
    `__DWARF,__weavec_defs` with the `debug` attribute, on ELF
    `.weavec.defs` marked `SHF_EXCLUDE`. A shared library's safe units are
    compared with each other when it is linked, not with the images that
    link against it: those are covered as one without the section is
    (unresolved question 7, (N-A3)). The record is compact (version 2): a
    header, the counts, then entries of 40 bytes (a kind; a key, the hash
    of the kind and the name; the hash; offsets into one table of the
    unit's strings, each string once). libzmq's 108 objects hold 11,988
    entries in 3.1 MB. The comparison walks each input's entries where its
    read put them, and an entry that agrees with the first of its name
    costs one lookup.
17. **After review (2026-11-21).** What a review of the implementation
    found, and what was done with it:
    - **Fixed.** An LTO object (bitcode, `-flto` and `-flto=thin`, and a
      thin archive's members) had its record read by nothing, so a link of
      LTO objects was not compared: its record is now its module's
      `__weavec.defs`, read lazily (`test/Driver/definitions-lto.cpp`).
      Each record ended in a zero byte, so an ELF partial link (`ld -r`)
      that joins records was read to its first: records now follow one
      another with nothing between them (`definitions-partial.cpp`). A
      record whose string offsets fall outside its strings is not read. A
      shift of a type whose width is not a power of two (`_BitInt(24)`)
      took its count unmasked: it takes the remainder. Division and right
      shifts kept `exact`, which Clang puts on the division of every
      pointer difference, so a difference that is not a multiple of the
      element's size was poison (J.2 item 45 says it is defined): they
      lose it. `llvm.memcpy.inline` is a move as `llvm.memcpy` is
      (`test/Guards/ir-defined.c`).
    - **Not fixed: limitations.** A Mach-O partial link keeps no record
      (amendment 16: the debug attribute that keeps records out of images
      drops them from `ld -r`'s output too), so its units are not
      compared. Vector division (`vector_size`, `ext_vector_type`) is not
      checked: Clang's extensions are on neither list, and
      `resources/safe/undefined.md` says so under _Beyond the lists_. A
      `WEAVEC_SAFE` C++ function in a unit compiled without
      `-fweavec-safe` that reaches its end stops with `unreachable`, not
      `missing-return`: Clang's check of the end (`SanitizerKind::Return`)
      is asked for in a safe unit, whose options the driver sets before it
      knows which functions are safe; either stop ends the program. The
      runtime's alternate signal stack is given back at a thread's end
      only while it is the thread's: one a program replaces with its own
      is kept (256 KiB per such thread).

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset) on
2026-11-20, against main at `2d56b9f1` (built in `../weavec-rfc47-main`),
and in the Linux container (aarch64, libstdc++, glibc). The evidence is in
`build/rfc51/` (not committed; `README.txt` there says what each file is):
`g3.sh`, `g4/` (`build.sh`, `time.py`, `times.txt`, `zmq/`), `g5.sh`,
`g6.sh`, `g8/` (`g8.py`, `irdiff.py`), `g7/`, `linux.sh` and its logs.

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass.** `test/Guards/ir-defined.c`, `ir-defined.cpp`, `unreachable.cpp`, `missing-return.c`, `missing-return.cpp`, `division.c`, `stack-exhaustion.cpp` (each kind's report line; a thread with a 256 KiB stack and recursion on the main thread); `test/Safe/assumed.c`, `assumed.cpp`, `signal-handler.cpp`, `bool-bytes.cpp`, `defined.c`, `defined.cpp`, `fix-assumed.cpp`, `undefined-translation.c`, `undefined-translation.cpp`; `test/Driver/safe-flags.cpp`, `definitions.cpp`, `safe-sanitizers.cpp`; `LibrarySpecTest.RowsNameSignalHandlers`; CTest `undefined-list`. `ctest --preset dev` passes 55 of 55, lit 176 of 176; the Linux container's `ci-release` 55 of 55. |
| G2 The list is closed (D1) | **Pass.** 221 C items and 80 C++ entries, each with a disposition of §1 (and amendment 1's two) and a test that exists; none says "undefined" (`scripts/check-undefined.py`). |
| G3 The probes | **Pass.** All 16 programs of `build/rfc51/probe` are rejected when they compile or link, or stop with a report, in the fast and the checked build: 32 of 32 (`g3.sh`): `missing-return`, `unreachable`, `stack-exhaustion` (three), `division-by-zero`, `definition-mismatch` (two), `assumed`, `pointer-bytes`, `union-pointer`, `signal-handler`, `pointer-cast` and Clang's `-Winvalid-noreturn`, `-Wformat`, `-Wformat-security`. |
| G4 Cost (D8) | **Pass.** Fast, against main's fast build, median of seven interleaved: RFC 0048's `json` port 0.99, RFC 0049's 1.00, the LRU port 1.00, tinyxml2 1.00, yaml-cpp 1.02, the interpreter 1.00, the key-value server 1.08 and 0.95 in two runs (its benchmark writes a log; 21 runs the second time), jsmn 1.01, sds 0.99, cJSON 1.01, inih 1.00, tinyexpr 1.01 (user time). Compiles: tinyxml2 1.006, the `json` port 1.013, the interpreter's largest file 1.016. The comparison at the link (amendment 16), median of 50 interleaved links of libzmq's 108 objects with and without `-fno-weavec-definitions`, timed in the driver: 0.11 ms of a 95 ms link (0.11%) with a `Release` build of the compiler (`build/rfc51/rel`), 0.9 ms (0.9%) with the `dev` preset's `Debug` build; the whole link's medians differ by less than their noise (-2.0%, +1.4%, +0.3% in three rounds of 25). The read the comparison shares with the allocator's check is 1.2 ms either way. It was 30 ms when first built, and 4 ms (4.3%) before amendment 16. |
| G5 Natural code | **Recorded.** The two trial programs as written: one new error (the key-value server's `sigaction` in a safe function; its fix, `WEAVEC_GUARDED` on the function that installs the handler, is one token). RFC 0044's 90 and RFC 0046's 40 correct files: none. Safe C's five conversions, tinyxml2, yaml-cpp, both `json` ports and the LRU port: none. libzmq (RFC 0049's conversion, 50,000 lines): 10, all `pointer-cast` for one idiom (a derived engine's member functions stored in the base's member pointer, applied to `this`, which is the derived object: correct code the rule cannot tell from the wrong use), and no `definition-mismatch` once amendment 9's two false ones were fixed. nng (65,000 lines of C): 6 (five `pointer-bytes` for a `bool *` given as `void *` to a safe getter that writes through it, one `union-pointer` for a tagged union with a `bool` member): correct code. None of these has a certain fix; no `assumed` error arose in any of them, so `--weavec-fix`'s removals were tested only by `test/Safe/fix-assumed.cpp`. |
| G6 No false stop | **Pass.** Both builds, no report of a new kind: the interpreter 37 of 37, the key-value server 289 of 289, RFC 0049's `json` port 645 of 646 (the one case no program passes: `check.py` decodes stdout with `replace`, and the case expects a lone surrogate), tinyxml2 517 of 517. |
| G7 The loop is not worse | **Pass.** The `json` port by a fresh agent with the frozen compiler: 2 rounds to a clean build, 6 min 23 s to `check.py`'s result (RFC 0050: 2 rounds, 5 min 17 s); no rule error in any round; it put its internal types in an unnamed namespace, as `definition-mismatch` advises. The interpreter made safe again by a fresh agent: clean at the third compile, about 6 minutes to both builds passing (this RFC's trial: the second, 6 minutes), the same five false rejections the first agent recorded (none from this RFC). |
| G8 Nothing elsewhere | **Pass.** 1,218 compiles of RFC 0044's and RFC 0046's sets, `test/cases` and `test/Guards` with main's compiler and this one: 532 identical objects, 665 differing only in the constants that carry the runtime's ABI (the frame descriptors' magic word and the ABI number; their IR is otherwise identical, `irdiff.py`), 19 differing that have safe functions, 2 that fail with both. The `dev` tests, the cases, the documentation's examples and the site's build pass; the Linux container's `ci-release` passes. |
| H1 Hygiene | **Pass.** The library from 31,844 lines (26,392 of code) to 34,169 (28,226), +2,325 (+1,834); the runtime from 10,831 (8,553) to 11,078 (8,744), +247 (+191). |

What the numbers say about the owner's goal: a safe program's every
compile now means what the guarantees page's first sentence says, item by
item of both languages' lists, at no measurable run-time cost and with one
new error in 7,354 lines of natural code, and a comparison at the link
that costs a tenth of a millisecond of libzmq's. The rules that close the C++ list's
member-pointer and base-destructor rows, and C's `bool`-through-`void *`
and tagged-union rows, reject correct code in the two large conversions:
the price of closing the list without an analysis. The cost the trial
measured is the next work: the interpreter as made safe runs at 1.9 times
the interpreter as written, and the second agent found that a key function
per node class turns its 41 `dynamic_cast`s into one comparison of vtable
pointers each (1.93 to about 1.6), which a checked cast (_Future work_)
would give without the edit.
