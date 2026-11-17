# RFC 0048: Safe code without the shadow

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-15
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Carries out what
  [RFC 0045](0045-type-stable-memory.md) designed and did not build (its
  §4, rule 4: a pointer loaded from a member is believed) and what it
  deferred (_Future work_: "Frames", "Views without guards", "The
  allocator's path"), and answers gate G5 of
  [RFC 0047](0047-conversion-in-place.md), which failed. Supersedes RFC
  0045's (Y3) and its amendment 8 for single pointers, strings and the
  index of a view; RFC 0047's amendments 3 and 17 and the last paragraph
  of its §1.4 ("a C++ object is not moved to the heap behind the program's
  back"); and, for how it is done, the heap placement of
  [RFC 0041](0041-safe-c.md) (S4). Corrects (X6) of
  [RFC 0044](0044-safe-cxx.md) as RFC 0047 restated it, which is false for
  a pointer into a returned frame. Extends `escape`, `call-args` and
  `index-single`, the enforcement ledger and the audit. Adds no
  annotation, no rule and no report kind; `WEAVEC_ESCAPES` gains two
  places it may be written. The runtime's ABI becomes 45.

This RFC was written at the request of the project owner on 2026-11-15,
who asked an eighth time for the single change that would do most to make
an AI rewrite into C or C++ as attractive as an AI rewrite into Rust, to
be implemented next, with breaking changes and the removal of old code
welcome. The measurements in _Motivation_ were taken on main at
`59a4bd71` (RFC 0047 merged), darwin-arm64, the `dev` build, Homebrew
LLVM 23.1.0. The programs, their builds and the samples are in
`build/rfc48/` (not committed; `README.txt` there says what each file
is). Nothing of the design has been built or prototyped; stage S0 is
where it meets code.

The owner accepted the design as drafted on 2026-11-15, before any of it
was built, and with it the seven decisions that were theirs to make. They
are marked **(D1)** to **(D7)** where they are argued, and collected
here. The thresholds of §13 and the _Unresolved questions_ are still
open, and S0 is where they are settled; what it measures may reopen
(D2), (D3) and (D6).

- **(D1) In the fast mode a safe function is checked against what it
  holds, and the rules carry weight.** A type, a bound, a length: never
  the shadow. Where a rule or typed memory already says an access is
  inside its object, no guard says it again. The shadow is read where a
  pointer arrives from code that is not safe, and nowhere else in safe
  code. `-fweavec-safe-checks=full` keeps every guard, and `verify`
  keeps each one removed as a monitor. This is how Rust does it, and it
  is the decision RFC 0043 and RFC 0045 declined to take for lifetimes.
  It is taken here for what type-stable memory makes harmless, and for
  the stack, where a rule is added first.
- **(D2) A `T *` in safe C++ points to one object, as in C.** A position
  that may be the end of an array is not passed, returned or stored as a
  plain pointer: its declaration says `WEAVEC_ENDED_BY`, or
  `WEAVEC_COUNTED_BY(0)` for an end.
- **(D3) A parameter that is kept says so, in C++ as in C.** A safe C++
  function stores the address of what a pointer or reference parameter,
  or `this`, refers to only if the parameter is `WEAVEC_ESCAPES` or its
  class is `WEAVEC_ALIASED`. This is a breaking change: code that
  compiles as safe today gets `escape` errors.
- **(D4) A local whose address is kept lives in typed memory.** It gets a
  slot from the pool of its type for its scope, in C and in C++, in
  every build. Every other local of a fast safe function lives in a
  plain frame: no redzone, no poisoning.
- **(D5) Without optimization the checked mode is the default.** `-O0`
  with `-fweavec-safe` means `-fweavec-safe-checks=full`; the fast mode
  starts at `-O1`. An agent's debug build is then the checked one with
  no flag, as a debug build of Rust checks more than a release build.
- **(D6) Iterators keep their guards for now.** The index of a view and
  the element of a range-based `for` lose theirs; an iterator held in a
  variable or handed to an algorithm does not. It is the one place this
  RFC leaves the shadow in safe code.
- **(D7) The gate is the one RFC 0047 failed, and S0 decides before the
  rest is built.** 1.3 times Clang, measured on the two conversions and
  the two ports. S0 prototypes the removal behind a hidden switch and
  counts the new errors; either number can reopen (D2) and (D3). The
  library's line limit rises from 26,500 to 27,400; the runtime stays
  within 11,000.

## Summary

Seven RFCs built Rust's loop in C and C++, and by their own gates the
loop is there: an agent converts a library in place in the time another
rewrites it in Rust, in as many rounds. What it gets for that is not what
the Rust agent gets. The Rust port of tinyxml2 runs at the speed of the
C++ original or faster. The converted original, 94% of its functions
safe, runs at 1.72 times: 6% faster than the same library with no safe
function at all. yaml-cpp, 99% safe, runs at 2.07 times. RFC 0047 set
the gate at 1.3, and both conversions failed it.

The reason is one design decision. A safe function is checked when it
compiles, and then nearly every access it makes is checked again when it
runs, against the shadow, as if the rules had said nothing: the byte a
string cursor is on, a node reached through a pointer, a comparison of
two strings the types already terminate. With the same objects compiled
without those checks and still on WeaveC's allocator, tinyxml2 runs at
1.14 times Clang.

And the second check is not the guarantee it was kept for. A safe C++
function may store the address of a caller's local in a pointer member.
After the caller returns, a write through that pointer lands in whatever
frame is there now: a probe below forges a pointer that way, with no
error and no report, in the fast build, the checked build and the
guarded build.

This RFC makes safe code pay what Rust pays:

- **Typed memory in C++.** A `T *` a safe function loads or receives is
  null or points to memory of type `T`, a bounded pointer has its
  elements, a string has its terminator. Accesses through them have no
  guard; an index is compared with a bound the function holds.
- **The stack, typed where it must be.** A parameter that is kept is
  declared (`WEAVEC_ESCAPES`, C's rule), and a local whose address is
  kept gets a slot from the pool of its type. A stale pointer to it then
  reaches memory of its type in the fast build and stops with a report
  in the checked build, every time. Every other local is in a plain
  frame.
- **Views and library calls.** The index of a `std::span` or a
  `std::string_view` is the library's comparison and nothing else, the
  element of a range-based `for` is believed, and a C library call whose
  arguments have bounds is compared with them and called directly.
- **The allocator's typed path**, shortened until an allocation and its
  release cost about what the system allocator's do.

Nothing changes for guarded code. The checked mode keeps every guard and
gains the stack.

Expected result (gates, §13): tinyxml2's conversion at no more than 1.25
times Clang (1.72 today), yaml-cpp's at 1.4 (2.07), the `json` port at
1.4 (1.73); no guard left in the conversions' safe functions but on
iterators and at the border; the probe below a compile error, or a
report in the checked build; and the two conversions brought back to a
clean compile with a counted number of new annotations.

## Motivation

### The loop is at parity and the result is not

| | Agent time | Rounds | Run time, times Clang |
| --- | --- | --- | --- |
| tinyxml2 rewritten in Rust (RFC 0047 G4) | 1,145 s | 4 | about 0.8 to 1.0 |
| tinyxml2 converted in place | 1,201 s | 3 | 1.72 |
| `json` port, Rust (RFC 0044 G4, RFC 0045 G5) | 7 min 29 s | 1 | 0.93 |
| `json` port, safe C++ | 9 min 7 s | 3 | 1.73 |
| yaml-cpp converted in place | about 20 min | 18 | 2.07 |

A team that chooses C++ over Rust chooses it for the code it has and for
speed. The first column says WeaveC can keep the code. The last says it
gives the speed away, and that is the column read after the work is
done. RFC 0045 moved the `json` port from 3.05 to 1.73 and said of what
was left: "No one cost dominates." That was true of a port that
allocates five million small objects. It is not true of the conversions.

### Measured: what 94% safe buys

`build/rfc48/tx/bench.cpp` parses tinyxml2's `dream.xml` 2,000 times
and walks the root's children. Milliseconds, best of three interleaved
runs:

| Build | ms | Times Clang |
| --- | ---: | ---: |
| The original, `clang++ -O2` | 723 | |
| The conversion, `clang++ -O2` | 706 | 1.00 |
| The conversion with no checks (`-fweavec-checks=none` objects), linked to the runtime and its allocator | 804 | 1.14 |
| The conversion, fast (416 of 444 functions safe) | 1,215 | 1.72 |
| The original, guarded (no safe function) | 1,288 | 1.78 of its own |
| The conversion, checked (`full`) | 1,352 | 1.91 |

The annotations cost nothing (706 against 723). Being safe buys 6% over
being guarded. Believed pointers are worth 80 ms of that and pools 25
(`-mllvm -weavec-believe=0`, `-weavec-pools=0`). Between the fast build
and the build with no checks lie 411 ms, 81% of the overhead, and all of
it is checks in the unit's own code.

yaml-cpp (`build/rfc47/g3/ybench/bench.cpp`: load, walk and emit a
160,000-line document, 4.68 million allocations): Clang 542 ms, no
checks on the runtime's allocator 828 (1.53), fast 1,121 (2.07), guarded
2,217 (4.09). Here the checks are half of the overhead and the allocator
the other half.

### Measured: where the time goes

The fast unit's ledger: 1,264 accesses, 837 proven, 427 guarded. 51 of
the guards are in the 28 functions the conversion left guarded (the
arena). The other 376 are in safe code:

| Guards in safe functions | Count | The line is typically |
| --- | ---: | --- |
| A load or a store through a pointer | 273 | `while ( *p && IsNameChar(*p) )`, `node->_parseLineNum = …`, `insertThis->_parent` |
| A C library call sent to the runtime's checked version | 43 | `strncmp( p, q, nChar )` |
| A reference test on a loaded pointer | 23 | `_mem[i] = _mem[_size - 1]` |
| Conversions, copies, entries | 37 | |

Sampled against the Clang build (`sample-fast.txt`, `sample-clang.txt`;
1,230 ms against 752 in those runs), the overhead divides roughly as: the
parser's functions 205
(the current byte of a string, a node reached through a pointer); the
runtime's `strncmp` 110 more than the C library's own; the node graph
and `DynArray` 100 (pointers loaded from members); the arena's guarded
functions 50; the allocator, zeroing and six sweeps 55.

In yaml-cpp the two instantiations of `RegEx::MatchUnchecked`, safe code
over a vector and a character source, take 411 ms by Clang and 731 fast:
half of the overhead in one function. The runtime takes 184 ms where the
system allocator takes 72 in the Clang build: 39 ns for an allocation
and its release. 2.8 million of the 4.68 million typed releases overflow
their pool into the quarantine.

Safe C has the same shape. RFC 0045's cJSON conversion, sampled
(`sample-cjson-F.txt`): 38% of its samples are in the runtime (the
allocator, the checked `strncmp`, `shadowLive`, the sweep, and
`pthread_get_stacksize_np`, which `__weavec_rt_escape_check` asks at
every call) and 21% in the program's own code.

### The checks say what the rules said

Each of the three kinds of guard that cost repeats something the
function was already compiled against.

- **The current byte of a string.** `string-index` lets a
  `WEAVEC_NULL_TERMINATED` pointer be read where it stands and stepped
  only over a byte that was tested. Safe C has compiled that read with
  no guard since RFC 0042 ((S1): "at a string's current byte, which is
  at or before its terminator"). RFC 0047's amendment 17 gave safe C++
  the guard back, because the alternative then on offer scanned the rest
  of the document at every call.
- **A member of what a pointer points to.** `node->_parent`, at a
  constant offset inside one `XMLNode`. In Safe C an unannotated pointer
  points to one object (W1) and the access has no guard. In safe C++ "a
  raw pointer parameter is not believed" and "a pointer loaded from a
  member keeps its guard" (RFC 0045, amendment 8), although the blocks
  are type-stable and RFC 0045's §4 lists both as believed.
- **`strncmp(p, q, n)`** on two pointers whose declarations say they are
  strings. Clang calls the C library. WeaveC calls the runtime, which
  compares byte by byte and then checks, against the shadow, the bytes
  it read.

### Measured: a hole where the stack is

`build/rfc48/probe/stack-type.cpp`, safe C++, no error (the three
functions are `noinline`):

```cpp
struct Counter { long hits = 0; long pad[3] = {}; };
struct Name { const char *text = "ok"; long pad[3] = {}; };
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &c) { counters.push_back(&c); }
  void bump() { for (Counter *c : counters) c->hits += 0x1000000; }
};
void enroll(Registry &r) { Counter local; use(local); r.add(local); }
const char *victim(Registry &r) {
  Name name;                    // laid over enroll's 'local'
  use(name);
  r.bump();                     // adds to 'name.text' through the stale Counter *
  return name.text;
}
```

`main` calls `enroll`, then `victim`, and prints the pointer it got and
the literal's address. Built with `-fweavec-safe-checks=full`, and built
with no safe function at all, it prints two addresses 0x1000000 apart
(`0x1057fa3af 0x1047fa3af` in one run) and reads through the first: a
pointer safe code forged, with no report.
AddressSanitizer with use-after-return detection stops it. The fast
build of this file happens to stop at a redzone; the fast build of
`stack-ref.cpp`, the same program with a `Counter` in `victim`, prints a
count a dead frame's pointer wrote.

Two sentences of the guarantee are wrong about this. (X6), as the
guarantees page restates RFC 0047's, lists "a reference (a raw pointer
kept in a member or an element)" among what is left and says: "Under
`-fweavec-safe-checks=full` all of them stop when they run." (U) stops
a use after return only "until another frame with tracked locals is laid
over those bytes". The heap has had an
answer to this since RFC 0036 and a cheap one since RFC 0045. The stack
has had redzones.

Safe C does not have the hole. Its `escape` rule lets a function store a
pointer parameter only if it is `WEAVEC_ESCAPES`, and a local whose
address goes to such a parameter is placed on the heap (S4). RFC 0044
gave C++ the half of `escape` that is about views of the function's own
locals; the half about parameters stayed in C, and RFC 0047 kept C++
locals in their frames on the strength of it.

### Why this is one change and not four

Removing the guards needs the stack to be typed: a pointer loaded from a
member can be believed only if it cannot lead into a frame that is gone.
Typing the stack needs the rule about kept parameters. With both, the
frames of safe functions need no redzones, which is RFC 0045's 0.10 s of
0.32. And once the checks in the code are gone, the allocator is what is
left, which yaml-cpp shows. The parts are sized by what the measurements
above attribute to each.

The machinery is mostly there. RFC 0042 built single pointers, field
annotations and conversions; RFC 0041 the placement of locals and the
split entry; RFC 0045 the pools, believed pointers and verify's
monitors; RFC 0047 brought bounds to C++. What is missing is the
decision.

## Soundness

### Typed memory, in C++

In a fast safe unit, for a program that meets the assumptions below,
every pointer a safe function receives (a parameter, `this`, a call's
result) or loads (from a member, an element, a global) satisfies its
declaration:

- **(V1) Single.** A `T *` without a bound is null or points to
  `sizeof(T)` bytes of memory of type `T`; a `T &` points to them.
  _Memory of type `T`_ is RFC 0045's term, extended: a live object of
  type `T` (or one with a `T` at that place); a released block of `T`'s
  pool, zeroed, or reused for `T` on the same element boundaries; the
  pooled slot of a placed local of that type (§5), in scope or not; or,
  for a pointer that was passed and not loaded, a local of a frame that
  is still live.
- **(V2) Bounded.** A pointer with a bound has the elements its bound
  gives, each memory of its element type. A `WEAVEC_COUNTED_BY(0)`
  pointer has none and is never accessed.
- **(V3) Terminated.** A `WEAVEC_NULL_TERMINATED` pointer has a zero
  byte at or after it, as (W3) says.
- **(V4) Views.** A `std::span`, a `std::string_view` and the standard
  containers hold a pointer and a count that satisfy (V2).

Then:

- **(V5) Accesses.** A safe function accesses memory through such a
  pointer at a constant offset inside `sizeof(T)`, at an index compared
  with the bound, at a string's current byte, or through a view's
  checked index, with no guard, and the access is inside memory of the
  pointer's type. Every other access keeps its guard (§6.3, §7).
- **(V6) No kept address of a plain local.** The address of a local, or
  of what a parameter refers to, is in memory safe code can load from
  only if that local is placed, or the parameter was declared kept and
  its argument was a placed local, a heap object or a global (§4).

(X5) and RFC 0047's typed memory for bounded members are the special
cases of (V2) that exist today. (Y2) is (V1) for the pointers RFC 0045
believed.

### What keeps it

Each line is a rule or a mechanism that exists, unless marked new.

| What could break typed memory | What prevents it |
| --- | --- |
| A block reused as another type | Pools (Y1). |
| A pointer made from an integer, a cast or a union | `int-to-pointer`, `pointer-cast`, `union-pointer` (X5). |
| A position past the end taken for an object | **New (D2):** a stepped pointer is not converted to a plain one (`call-args`, §2); `&p[i]` is compared. |
| A pointer to a local that outlives it, in memory | **New (D3, D4):** `escape` for parameters, and placement (§4, §5). |
| A borrow or a view of a plain local that outlives it, in a local or a result | `stale-borrow`, `escape` (X3, X4), the result check (Y4), frames that share no bytes (RFC 0045, amendment 10). |
| A count changed without its pointer | `counted-pair`, `field-address`. |
| A view made from a pointer and a count that exceeds it | `call-args` at compile time; **new:** a comparison where the view is made (§6.1). |
| A terminator written over while a string points at it | `string-write`, `stale-borrow`'s rule S, `call-alias`; the zero byte after a heap block (W3). S0 (d) audits the list. |
| Storage that changes type inside a live block | (Y6): `stale-borrow`, `call-alias`, `owner-path`, rule K, `escape`. |
| An uninitialized pointer | Zero initialization, which the driver adds. |
| Two threads, one writing | (Z6). |
| Code that is not safe | The entry's checks against the shadow (§7); X-A6, Y-A1 and the two assumptions below. |

### What this does for the earlier guarantees

- **(X6), corrected.** A temporal error in a safe C++ function reaches,
  through any path (X6) lists, memory of the pointer's type in the fast
  mode, and stops with a report in the checked mode. That now includes
  a pointer to a local: `stack-use-after-scope`, by (T), since a placed
  local is a heap block. The sentence RFC 0047 wrote becomes true.
- **(U)** is no longer what stands behind a safe function's locals. It
  stays for guarded code.
- **(Y3)** shrinks to: an access through an iterator that is not a
  range-based `for`'s, and through the result of a library function that
  is neither trusted nor in the table, is guarded.
- **(O4)**, the test of a reference when it is loaded, is the checked
  mode's. In the fast mode it already passed for a reused block; it is
  not made.
- **(M4)** stands, and gains C++ pointer parameters (§7).

### What gets weaker, in plain terms

In the fast mode, for safe code:

- **A stale pointer to a released block that has not been reused** no
  longer stops at a guard. It reads the zeroed block, which is memory of
  its type. The fast mode never promised that trap (it depended on
  whether the slot had been reused), and the checked mode keeps it.
- **The shadow no longer stands behind the rules** for the accesses
  above. If `string-index` has a hole, a string is read past its
  terminator with no trap. If `escape` has a hole, a pointer into a
  frame that is gone can be written through, and a plain frame holds
  return addresses. Today a guard might catch either; the probe shows it
  also might not. `verify` monitors every guard removed (§9), and gate
  G3 runs every seeded bug this project has under it.
- **Typed memory that code which is not safe broke** is found at the
  entry it comes in by, or not at all: a wrong pointer written into a
  member by a guarded function is believed by the safe function that
  loads it. In Safe C that has been so since RFC 0042 (T-A1).

Nothing gets weaker in the checked mode or for guarded code, and the
checked mode gets stronger: a kept local is now watched as a heap block
is.

### Bugs caught

When it compiles (each an existing rule's new case):

```cpp
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &c) { counters.push_back(&c); }           // escape
};
class Slice {
  const char *WEAVEC_COUNTED_BY(size_) data_; std::size_t size_;
public:
  const char *end() const { return data_ + size_; }          // call-args
  const char *WEAVEC_COUNTED_BY(0) limit() const { return data_ + size_; }
};
char past(const Slice &s) { return *s.limit(); }             // index-single
void hook(Bus &bus) { bus.keep(Widget()); }                  // escape: a temporary
```

When it runs: the probe, once `add` says `Counter &WEAVEC_ESCAPES c`,
stops in the checked build with `stack-use-after-scope` naming `local`
of `enroll`, and in the fast build adds to a `Counter` and prints the
literal's address. A `std::span(p, n)` with `n` past `p`'s bound stops
where it is made (`pointer-out-of-bounds`). A `WEAVEC_ESCAPES` argument
from code that is not safe that points into a stack stops at the entry
(`stack-escape`), as in C.

### Bugs deliberately not caught

- **A stale pointer in the fast mode**, as RFC 0045 says: it reaches
  memory of its type. Now for locals too.
- **An iterator out of range** where no guard catches it today:
  `m.find(k)->second` on a map without the key reads the map's end
  node. Unchanged; _Future work_.
- **What code that is not safe does to typed memory** between entries.
- **A leak of a placed local's slot** when `longjmp` or `pthread_exit`
  leaves its scope: memory, not safety.

### Accepted rejections and false traps

- **A function that keeps the address of what a parameter refers to**
  needs `WEAVEC_ESCAPES` on it, and so does each caller that passes its
  own parameter on. The annotation spreads up a call chain as
  `WEAVEC_LIFETIMEBOUND` does, and stops at the first local, heap object
  or aliased class.
- **`std::max(p, q)` on two pointers to locals** binds them to reference
  parameters, which exposes them (§4): the locals are placed. Correct,
  and a wasted allocation.
- **A function that returns a cursor or an end as a plain pointer** is
  rejected until its result says what it is.
- **A temporary given to a parameter that keeps** is rejected.
- **`stack-escape` at an entry**: a guarded function that passes its own
  parameter, which points into a caller's frame, to a safe function's
  `WEAVEC_ESCAPES` parameter. The fix is the same annotation on the
  guarded function's parameter, which makes its callers place the local.
- **An empty counted pointer converted to a plain one** stops
  (`bounds-conversion`), as in C.

### Assumptions

X-A1 to X-A7, Y-A1 to Y-A5 and Z-A1 to Z-A5 stand. Y-A1 now covers
pointers as it covers references: the entry's check (§7) finds a pointer
that is not addressable, not one to an object of another type. Added:

|          | Assumption |
| -------- | ---------- |
| **V-A1** | Code that is not safe stores in a pointer member, an element or a global that safe code loads only null or a pointer to a live object of the declared type that is not a local, and passes for a parameter that keeps only such an object or a local `weavec-cc` placed. The audit lists the functions that are not safe and have a parameter that keeps. |
| **V-A2** | Code not compiled by `weavec-cc` that is handed a pointer to a plain local keeps it no longer than the call (S-A2, now for C++). |

## Detailed design

### 1. Modes (D5)

`-fweavec-safe-checks=bounds` is the fast mode and `full` the checked
one, as now. The default becomes `full` when the unit is compiled
without optimization and `bounds` otherwise; the flag, when given,
decides. A unit is fast only under `-fweavec-safe` (RFC 0047, amendment
12). Everything below that says "fast" is skipped under `full`, except
§4 (a rule) and §5.1 (placement), which hold in every build.

### 2. Single pointers (D2)

#### 2.1 Belief

`annotateFields` gives every raw pointer and reference member of a class
a safe C++ function names its kind and its pointee's size (RFC 0047's
amendment 6 gave them to bounded members only). In a fast safe function
an access at a constant offset inside `sizeof(T)` has no guard when its
pointer is:

1. a pointer parameter (behind the null test of RFC 0035 §2.4 when the
   offset is beyond the guard page), a reference parameter or `this`;
2. loaded from an annotated member, an element of a container or array
   of pointers, or a global;
3. the result of a safe function, of a library function a table row
   says returns one object or null (`std::unique_ptr::get`,
   `std::get_if`, `std::addressof`; rows to add), or `&p[i]` after its
   comparison;
4. the result of a project function that is not safe, after one range
   guard of `sizeof(T)` bytes where it arrives (§7).

The ledger's reason is `typed`. No reference test is placed.

#### 2.2 The conversion rule

`call-args` gains a case in C++: a pointer the function stepped or
offset (`p + n`, `p++`, `&a[0] + n`, an array's end) is not returned,
stored in a member, an element or a global, or passed, where the
declaration gives no bound. The accepted forms:

- the destination declares a bound: `WEAVEC_ENDED_BY(limit)` for a
  cursor, `WEAVEC_COUNTED_BY(n)` for what is left, and the conversion is
  compared (RFC 0047 §1.4);
- the destination declares an end, `WEAVEC_COUNTED_BY(0)`: no
  comparison, and the pointer is from then on a position: compared,
  subtracted, passed on, never accessed (`index-single` at compile time,
  since its bound is the constant zero);
- `&p[i]`: the index is compared with the bound (`pointer-out-of-bounds`)
  and the result is a pointer to one object;
- a bounded pointer that was not stepped: it points to its first
  element, after one comparison that it has one (`bounds-conversion`),
  as RFC 0042 §6 does in C.

Safe C keeps its conversions as they are (checked when the program
runs); the compile-time case is C++'s, where RFC 0047 found the idiom.
S0 (b) counts what it rejects.

### 3. Strings

In a fast safe function, in both languages:

- A read of a `WEAVEC_NULL_TERMINATED` pointer at its current byte, or
  at a constant offset `string-index` accepted, has no guard (reason
  `string`). Safe C++ gets what Safe C has.
- A value the checker has shown to be a string (a string parameter,
  member or result, a literal, `c_str()`, and what `string-index` lets
  be made from them) is not searched again where it is passed, returned
  or stored as a string. The checker marks the conversions that do need
  a search: a counted buffer or an array given where a string is
  declared, searched inside its bound (`unterminated-string`). RFC
  0047's amendment 17 measured the alternative at 150 times.
- At the validating entry (§7) a string is checked in constant time
  where that is enough: a pointer into a live heap block has a
  terminator by (W3), which counts the zero byte after the block. Only a
  pointer into a local or a global is searched to its object's end.

### 4. Kept parameters (D3)

`escape`, in a safe C++ function, gains C's clause. Call _the referent_
of a parameter what a pointer or reference parameter, or `this`, points
to, and _an address in it_ the referent's own address, the address of a
member or an inline array element of it, a lambda that captures it by
reference or captures `this`, and a view of it. An address the function
reaches through a load (`p->next`, an element of a member vector, what a
`std::unique_ptr` member owns) is not one: it is in typed memory
already.

- **Kept.** An address is _kept_ when it is stored in a pointer member
  or element of any object, in a global, or through another parameter;
  when the address of a pointer variable that holds it is taken or bound
  to a reference (`v.push_back(&c)` binds a temporary pointer to a
  reference parameter); or when it is an argument for a parameter that
  keeps. A parameter _keeps_ if it is `WEAVEC_ESCAPES`, a pointer or
  reference to a `WEAVEC_ALIASED` class, or a callable taken by value
  (RFC 0044 §8).
- **The rule.** A safe function keeps an address in a parameter's
  referent only if that parameter is declared `WEAVEC_ESCAPES` or its
  class is aliased. `WEAVEC_ESCAPES` is written after the `*` or the
  `&`, and after a member function's parameter list for `this`. Every
  declaration of a function, and every function that overrides a virtual
  one, says the same (`bad-annotation`).
- **Not kept.** A local pointer or reference, an argument passed by
  value as a pointer or bound to a reference parameter that does not
  keep, and a member of a view (a reference member, a `WEAVEC_BORROWED`
  pointer, a `std::string_view`): the rules follow those (X3, X4). An
  address loaded from a view parameter counts as an address in that
  parameter's referent.
- **Locals.** Keeping the address of the function's own local, by-value
  parameter, or anything inside one, is not an error: the local is
  placed (§5). The existing clause that let "a local keeper" hold a
  local's address as a view goes; such a local is placed like any other.
- **Never kept.** A temporary, and a `thread_local` variable, whose
  storage ends with its thread and is not the runtime's: `escape`.

An aliased class needs no `WEAVEC_ESCAPES`: RFC 0047 defines it as a
class whose objects "are changed through pointers that other objects
keep", so its objects are kept by declaration, and its locals are placed
by type. tinyxml2's back pointers (`_document`, `_parent`) are of
classes its conversion already declared aliased; its cursors, which
`StrPair::Set` stores, need the annotation along the parser chain, about
as many as its 22 `WEAVEC_LIFETIMEBOUND`.

### 5. Placed locals and plain frames (D4)

#### 5.1 Placed locals, in every build, in both languages

A local variable, a by-value parameter or a temporary is _placed_ when:

- its type is, or holds by value (a member, a base, an array element,
  the storage of a `std::optional`, `std::array`, `std::pair`,
  `std::tuple` or `std::variant`), a `WEAVEC_ALIASED` class; or
- its address, or an address inside it, is kept (§4); or
- in C, RFC 0041's condition holds (`mayOutlive`).

A placed local gets a slot for each of its scopes and gives it back when
the scope ends on any path, after its destructor, in a cleanup when an
exception unwinds. `longjmp` and `pthread_exit` leak the slot.

- In a fast unit the slot comes from the pool of the local's type (keyed
  as RFC 0045 §2 keys an allocation) and returns to it, zeroed.
- In a checked or guarded unit it is an ordinary block, released to the
  quarantine. A later access through a kept pointer is a report every
  time, of kind `stack-use-after-scope`, with a line that names the
  local, its function and why it was placed.

The checker marks what it decided (an implicit annotation on the
declaration, and on the constructors and the functions that return by
value a type that holds an aliased class, for temporaries); the pass
places every alloca so marked, and also any whose address its own
analysis of the IR finds stored in memory or passed for a parameter that
keeps (`mayOutlive`, which C uses today, extended with the function
table's `escapes` for C++ and `this`). Guarded functions are placed by
the same marks and the same analysis: a guarded `main` whose
`XMLDocument doc;` is of an aliased class places it, with no annotation
and no error.

`placeOnHeap`'s `calloc` and `free` become the typed entry points.
Safe C's placed locals stop paying the quarantine.

#### 5.2 Plain frames, in a fast safe unit

A safe function's local that is not placed lives in the frame Clang
gives it: no redzone, no shadow written at its scope's start or end or
at the return. Frames still share no bytes (RFC 0045, amendment 10).
The exception is a local whose address is passed to code that is not
safe (a guarded project function, a library function that is neither
trusted nor bounded by the table, an indirect call the unit cannot
see): it stays in a tracked frame, as now, so that the callee's guards
have a redzone to find.

What stands behind a plain local is in the table of _What keeps it_:
§4 for memory, (X3), (X4) and (Y4) for locals and results. A safe or
trusted callee handed its address stays inside its type or its bound by
its own rules.

### 6. Views and the library (D6)

#### 6.1 The index of a view

RFC 0045 §5 trusts no member of a view. It now trusts the members of
`std::span`, `std::basic_string_view` and the contiguous containers'
views whose precondition the library's hardening checks: `operator[]`,
`front`, `back`, `first`, `last`, `subspan`, `substr`, `remove_prefix`,
`remove_suffix`, and the ones with no precondition (`size`, `data`,
`empty`). S0 (c) reads each in libc++'s `fast` mode and under
`_GLIBCXX_ASSERTIONS`; one that is not checked stays out by a table
row. A trusted member's access has no guard: the comparison the library
makes is the check, as it is for `std::vector::operator[]` today.

So that (V4) holds without the guard, a view made from a pointer and a
count (`std::span(p, n)`, `std::string_view(p, n)`, the table's
`counted` and `ended` clauses) is compared with the pointer's bound
where it is made (`pointer-out-of-bounds`). RFC 0047's amendment 5 left
that to the library's guarded accesses.

#### 6.2 The element of a range-based `for`

The dereference, the step and the comparison of the hidden iterator of a
range-based `for` over an array, a standard container, a `std::span` or
a string view are trusted call sites: the loop reads an element only
after comparing with the end, and `stale-borrow` already rejects a
change to the range inside the loop. A change through an aliased object
the rules do not follow leaves the loop in the old buffer, which is
memory of the element's type to its old end.

#### 6.3 What keeps its guard

An iterator held in a variable (`*it`, `it->second`, `it + n`), the
library's algorithms and every other library function that takes an
iterator or a raw pointer and is not in the table: compiled as today.
Gate G7 records how much of each conversion's remaining guards and time
this is.

#### 6.4 C library calls

In a safe function, a call the library table describes whose memory
arguments all have bounds the function holds, or are strings, is
compared with those bounds (nothing, for a string the row reads to its
terminator) and calls the C library's function, not the runtime's
checked version: `strncmp`, `strcmp`, `strlen`, `memcpy`, `memcmp`,
`memchr` and their kin. The ledger's reason is `in-bounds`. An argument
without bounds takes today's path.

### 7. The border

The shadow is read where a pointer arrives from code that is not safe.

- **The validating entry** (RFC 0041, amendment 18; reached from code
  that is not safe and by indirect calls) checks, in C++ as in C: a
  plain pointer parameter is null or its `sizeof(T)` bytes are
  addressable (new in C++; RFC 0047's amendment 3 left it out because
  ends were passed as plain pointers, which (D2) ends); a bounded one
  covers its count; a string as §3 says; a `WEAVEC_ESCAPES` argument
  does not point into a stack (`stack-escape`; new in C++). References
  and `this` are believed (Y-A1), as now. Safe callers call the body.
- **Results** of project functions that are not safe: a plain pointer
  gets one range guard where it arrives; a reference is believed.
- **`WEAVEC_GUARDED` functions** in a safe unit are guarded code: every
  guard, tracked frames, validating entries when they call in.
- The `stack-escape` test reads the thread's stack bounds once per
  thread, not once per call.

### 8. The allocator's typed path

(P1) to (P3) of RFC 0045 do not change. What changes is what an
allocation and a release of a typed block execute, which S0 (e) profiles
before choosing among:

- a release that decides from the slot word alone (live, typed, its
  pool) and pushes, instead of `shadowLive` and `weavecRtShadowObject`
  before `release`;
- an inline pop and push against the thread's pool cache for a site
  whose record already names its pool, with the runtime behind an empty
  or a full cache;
- a pool budget under which a steady stream of releases does not spill
  into the quarantine (2.8 million slots did in yaml-cpp), since each
  spilled slot waits for a sweep to return.

The target is gate G8: a typed allocation and its release within 1.5
times the system allocator's on a benchmark of pairs, and yaml-cpp's
time in the runtime within 1.5 times its time in the system allocator.
`WEAVEC_RT_ABI` becomes 45.

### 9. Verify mode, the ledger and the audit

- **Verify.** Every guard this RFC removes is kept as a monitor with the
  site flag `WeavecRtSiteBelieved`, which a released heap block and a
  released placed slot pass. Under `verify` a fast unit's frames are
  tracked as they are today, so a believed access that reaches a frame
  that is gone, a redzone or another object reports `weavec.proven`.
  That is a hole in a rule, found by a test.
- **The ledger** (version 11): the reasons `typed`, `string`, `view`,
  `range-for` and `in-bounds` on proven rows; a function's `keeps` (its
  parameters that keep, `this` included); a unit's `placed` list (the
  local, its function, and why: `aliased`, `kept`); the summary line
  ends `; <n> placed locals`.
- **The audit** (version 7): the functions that are not safe and have a
  parameter that keeps (V-A1); the placed locals, since each is an
  allocation the source does not show; the guards left in safe
  functions, by reason.

### 10. Safe C

Safe C already believes its pointers. It gains: typed slots for placed
locals (§5.1); strings not searched again (§3); C library calls compared
in hand (§6.4); the entry's constant-time string check and the cached
stack bounds (§7); the allocator's path (§8); and (D5). Its rules do not
change, and its plain frames are _Future work_: its locals keep their
tracked frames until its conversions are measured again.

### 11. Cleanups and breaking changes

- **Breaking:** `escape` for kept parameters and `call-args` for stepped
  pointers in safe C++ (D2, D3); `index-single` for an access through a
  `WEAVEC_COUNTED_BY(0)` pointer; `stack-escape` and a plain pointer's
  range guard at a C++ validating entry; a comparison where a view is
  made from a pointer and a count.
- **Breaking:** `full` is the default at `-O0` (D5); the runtime's ABI
  45; the ledger's version 11, the audit's 7; `weavec.h` 3.4.
- **Removed:** the guards of RFC 0047's amendments 3 and 17 and RFC
  0045's amendment 8 in fast safe C++; the reference test in the fast
  mode; `escape`'s "local keeper" clause; `placeOnHeap`'s `calloc` and
  `free`; the claim in (X6) that this RFC corrects; the sentences of the
  guides that explain which pointers are believed, which become one.
- **Not changed:** a unit with no safe function and no annotated
  declaration (it compiles to the same object but for the ABI number);
  ownership's and the threads' rules; (P1) to (P3); the sweep.

### 12. Documentation

- `resources/safe/rules.md`: `escape` (_In C++_ rewritten around kept
  parameters, with the probe as its first example), `call-args` and
  `index-single` (ends and cursors), `safe-cxx` (what a plain pointer
  is). Each example compiles under `docs/scripts/check-examples.py`.
- `docs/pages/reference/guarantees.md`: (V1) to (V6) in place of the
  believed-pointer paragraphs of (Y2) and (Y3); (X6) and (U) as
  corrected; V-A1, V-A2.
- The guides `safe-cxx.md`, `convert-cxx.md` and `safe-c.md`: "what a
  safe function pays for" (a bound, an index, an entry), "kept
  parameters and placed locals", and `-O0` as the checked build.
  `docs/annotations.md` and the command-line reference.
- `docs/roadmap.md`: a section for this RFC, with the implementation.

### 13. Stages and gates

- **S0 Before code.**
  (a) A prototype behind a hidden switch that removes, in a fast safe
  C++ unit, the guards of §2.1, §3 and §6 and the tracked frames of
  §5.2, with no rule behind them, run on tinyxml2, yaml-cpp, the `json`
  port and the LRU port. If the conversions are not within a tenth of
  their no-check builds (1.14 and 1.53), the design is wrong about
  where the time is.
  (b) The new `escape` and `call-args` cases, written as a survey over
  the two conversions as converted and the four corpus projects
  unedited: errors, and the annotations that would clear them. More
  than one annotation per hundred lines of a conversion reopens (D3).
  (c) The census of §6.1 in both standard libraries.
  (d) Every way a safe function can write a byte under a live string
  pointer, or a pointer value under a believed pointer, listed against
  the rule that stops it; a gap keeps that access's guard. And that the
  byte after a heap block, pooled or not, is zero, as (W3) counts on.
  (e) The allocator: a benchmark of pairs and a profile of §8's
  candidates.
  (f) Twelve programs of the kind of `stack-type.cpp`, written by hand
  (a kept address through a reference, `this`, a lambda, a
  pointer-to-pointer, a view, a temporary, a `thread_local`, a guarded
  caller), each with the outcome this RFC requires.
  (g) How Clang's temporaries and by-value parameters are marked for
  placement.
- **S1** Kept parameters in the checker; the marks. **S2** Placed locals
  in the passes and the runtime; plain frames. **S3** Single pointers:
  field annotations, belief, the conversion rule, ends, the entry.
  **S4** Strings. **S5** Views, range-based `for`, C library calls.
  **S6** The allocator. **S7** Verify, the ledger, the audit, the `-O0`
  default. **S8** The pages and guides; the two conversions brought back
  to a clean compile. **S9** The gates.

Gates (thresholds are proposals until S0):

- **G1 Tests.** `test/Safe/cxx-kept.cpp`, `cxx-ends.cpp` and new cases in
  `cxx-bounds.cpp`; `test/Guards/placed-local.cpp` (the exact
  `stack-use-after-scope` lines, checked and guarded), `ir-plain-frames.cpp`,
  `ir-typed-cxx.cpp`, `ir-strings.c`, `ir-library-in-hand.c`,
  `stack-escape-entry.cpp`; `test/Driver/safe-checks-modes.cpp` for
  `-O0`; `rt_test` `placed-slots`, `pool-pairs`; the ledger and the
  audit in `test/Driver/`.
- **G2 The stack.** `stack-type.cpp` and `stack-ref.cpp` as they are do
  not compile (`escape`). With the annotation, the checked build stops
  with `stack-use-after-scope`, the fast build prints the literal's
  address, and `verify` reports nothing. S0's twelve programs each do
  what was written beside them.
- **G3 Nothing believed is wrong.** RFC 0044's 110 seeded bugs and their
  twins, RFC 0046's sets, RFC 0047's probes, the three ports and both
  conversions' test suites, built fast under `-fweavec-checks=verify`:
  no `weavec.proven`. Built `full`: every seeded bug that compiles stops
  as it does today.
- **G4 The conversions convert again.** tinyxml2 and yaml-cpp, from RFC
  0047's results, brought to a clean compile by an agent with the pages:
  their test suites pass in both builds with no report; the share of
  safe functions falls by at most two points (93.7%, 99%); the
  annotations added and the agent's time are recorded.
- **G5 Cost.** Fast, times the same source by Clang: tinyxml2's
  conversion at most 1.25 (1.72 today), yaml-cpp's 1.4 (2.07), the
  `json` port 1.4 (1.73), the LRU port 1.15 (1.18). Recorded beside
  each: the Rust port's time, and peak memory.
- **G6 Safe C.** The conversions RFC 0045's G5 timed: none slower than
  on main; cJSON and sds recorded against their 2.22 and 2.04.
- **G7 Guards left.** In the two conversions' safe functions, the
  ledger's guarded rows are iterators (§6.3) or the border (§7), and no
  more than a tenth of today's 376 for tinyxml2.
- **G8 The allocator.** §8's two targets.
- **G9 Nothing elsewhere.** A unit with no safe function and no
  annotated declaration compiles to main's object but for the ABI
  number; the `dev` tests, the cases, the corpus gate and the
  documentation's examples pass; linux-aarch64 with libstdc++ passes.
- **G10 The survey again.** The four corpus projects, unedited: errors
  by rule beside RFC 0047's table; the rise in `escape` and `call-args`
  is the cost of (D2) and (D3) to code that has not been converted.
- **H1 Hygiene.** The library within 27,400 lines; the runtime within
  11,000.

## Annotation surface

`resources/include/weavec.h` becomes version 3.4. No macro is added and
none changes its expansion.

- **`WEAVEC_ESCAPES`**: as today after the `*` of a pointer parameter;
  now also after the `&` of a reference parameter
  (`Counter &WEAVEC_ESCAPES c`) and after a member function's parameter
  list, where it says the function keeps `this`
  (`void attach(Bus &bus) WEAVEC_ESCAPES;`). Both places parse today.
  It expands to `[[clang::annotate_type("weavec.escapes")]]` and to
  nothing under other compilers.
- **`WEAVEC_COUNTED_BY(0)`**: the existing annotation with a constant
  zero, documented as the way to declare an end.
- **`WEAVEC_ALIASED`**: unchanged; it now also means its objects are
  kept, so its locals are placed.

`docs/annotations.md` mirrors each; the constants in
`include/weavec/Frontend/SafeCheck.h` do not change.

## Diagnostics

No rule identifier and no report kind is added. Messages added, each
pinned in `test/Safe/`:

```text
error: the address of 'c' is stored in 'counters', and 'add' only borrows 'c' [weavec-safe:escape]
note: write 'Counter &WEAVEC_ESCAPES c' if 'add' keeps it; a caller's local then lives in typed memory
error: 'this' is kept by 'subscribe', and 'attach' only borrows it [weavec-safe:escape]
note: write WEAVEC_ESCAPES after the parameter list of 'attach', or declare 'Widget' WEAVEC_ALIASED
error: a temporary is kept by 'keep' [weavec-safe:escape]
error: 'cache' is thread_local, and its address is kept by 'add' [weavec-safe:escape]
error: 'data_ + size_' is a position, returned where a pointer to one object is declared [weavec-safe:call-args]
note: declare the result WEAVEC_COUNTED_BY(0) if it is an end or WEAVEC_ENDED_BY(limit) if it is a cursor, or return '&data_[i]'
error: 'limit' points to no element and is read [weavec-safe:index-single]
```

At run time, existing kinds in new places, pinned in `test/Guards/`:

```text
weavec: stack-use-after-scope at <file>:<line>:<col>: read of <n> bytes at <address>
weavec: <address> is <k> bytes inside '<local>' (<n> bytes), a local of <function> whose scope has ended; it was placed in typed memory because its address is kept (<file>:<line>)
weavec: stack-escape at <file>:<line>:<col>: argument <i> of <function> is kept and points into the stack of thread T<t>
```

The summary line of `-fweavec-summary` ends `; <n> placed locals`.

## Drawbacks

- **More rests on the rules.** RFC 0043 chose that nothing at run time
  would, and RFC 0045 listed "trust the rules" among its rejected
  alternatives with RFCs 0030 to 0034 as the reason. This RFC trusts
  them for spatial facts they state exactly (a type, a bound, a
  terminator) and for the stack, and keeps `full` and `verify` as the
  place a wrong rule is found. A hole in `escape` is a write into a
  frame that is gone.
- **New errors in code that compiled.** Both conversions must be touched
  again, and the survey's counts on unconverted code rise.
  `WEAVEC_ESCAPES` spreads along call chains.
- **Locals move.** A placed local costs an allocation per scope and is
  no longer near its frame; a program that compares stack addresses, or
  counts on a local's destructor running where `longjmp` skips it, sees
  the difference. The summary and the audit list them.
- **Two kinds of frame, three kinds of local** (plain, tracked, placed)
  in the passes, where there was one.
- **Iterators are left**, so iterator-heavy code keeps its cost and the
  principle has an exception.
- **The allocator work is engineering without a design to point at**,
  and yaml-cpp's gate depends on it.
- **A default that depends on `-O`** (D5) will surprise someone who
  benchmarks a debug build.
- **About 900 more library lines** and a limit raised an eighth time.

## Alternatives

- **Keep the second check.** Safe code stays within 6% of guarded code,
  and the Rust port stays about twice as fast.
- **Believe the pointers and leave the stack as it is** (RFC 0045's rule
  4 as designed, with the reference test). The probe is what that
  believes.
- **A test at each store of a pointer into memory** ("not into a
  stack") in place of `WEAVEC_ESCAPES`. No annotation and no new error.
  It stops correct programs (an object that registers itself and
  unregisters in its destructor), costs a comparison at every pointer
  store, and tells the agent when a test runs what a rule tells it when
  the file compiles.
- **Place every local whose address is taken.** No rule either, and an
  allocation for every `std::string s; f(s);`.
- **A second stack** for locals whose address leaves the function
  (SafeStack). It keeps return addresses out of reach of a stale
  pointer and does not make the stack typed: the probe would still
  forge its pointer. It could stand behind plain frames later.
- **Bounded iterators** (libc++'s ABI flags). They would end (D6)'s
  exception, change the layout of standard types across a program that
  mixes safe and guarded units, and do not exist in libstdc++.
- **Generations or tags in pointers** (MTE, top-byte ignore) to make a
  stale pointer stop in the fast mode. Not available on x86-64 without
  masking every pointer handed to code WeaveC did not compile.
- **Cheaper guards for everyone.** RFC 0037 measured it and declined.
- **Something other than speed.** Fix-its and a mode that applies them
  would shorten the loop, which is already at parity. A conversion of a
  project of 300,000 lines would say more about the offer than any gate
  so far, and is a measurement, not a design. A project tool in the
  manner of Cargo is a product of its own. None of them changes the last
  column of the first table.

## Prior art

- **Rust**: the model for (D1). Safe code pays for an index and nothing
  else; `unsafe` code owes safe code its invariants (V-A1); a debug
  build checks more than a release build (D5); a soundness bug in the
  compiler is a bug, not a reason to check twice.
- **Clang's `-fbounds-safety`**: `__single` is the default for a pointer
  in a declaration, arithmetic on it is an error, and a conversion from
  an indexable pointer to `__single` is checked. (D2) is that, with an
  end declared as a pointer to no element.
- **Go's escape analysis**: a variable whose address outlives its
  function is allocated on the heap, silently. (D4) is that, decided by
  a declared contract instead of an analysis across functions, with the
  heap typed.
- **AddressSanitizer's fake stack**
  (`detect_stack_use_after_return`): frames moved to the heap so that a
  use after return is found; it found the probe. It moves whole frames
  and reuses them by size class; §5.1 moves the locals that are kept
  and, in the checked build, keeps each until no pointer is left.
- **SafeStack and CPI** (Kuznetsov et al., 2014): the split of locals by
  whether their address escapes, for containment. See _Alternatives_.
- **Cyclone's regions and Checked C**: stack pointers that may not
  escape, by type. `WEAVEC_ESCAPES` is one bit of that, on the side
  that keeps.
- **Cling, Type-After-Type, `kalloc_type`** (RFC 0045): type-stable
  reuse, here extended to locals.
- **libc++ hardening**: the comparison §6.1 relies on, with documented
  coverage.
- **RFCs 0030 to 0034**: this project's record of removing checks on the
  strength of an analysis that inferred facts across functions. What is
  believed here is declared in a type or decided in one function, and is
  monitored.

## Unresolved questions

1. **Does the prototype reach the no-check builds?** S0 (a). If the
   guards on iterators and at entries are more of the time than the
   ledger suggests, (D6) is the wrong exception.
2. **How many annotations does (D3) cost?** S0 (b). The fallback is the
   store test of _Alternatives_ for parameters, keeping placement for
   locals.
3. **Indirect calls between safe functions** reach the validating entry.
   tinyxml2's virtual `ParseDeep` then pays three range guards a call.
   Is a second vtable slot, or a body at a fixed offset from the entry,
   worth its complexity?
4. **Temporaries and by-value parameters** (S0 (g)): can every one that
   holds an aliased class be found from its constructor or its `sret`
   argument, or does the checker have to reject some?
5. **The terminator's audit** (S0 (d)): a local array that becomes a
   string and is then written by index is the known suspect. Does the
   string keep the array's bound there, as a rooted pointer does?
6. **Trusted clones and plain frames.** A trusted library function is
   handed plain locals. Is every one of RFC 0045's trusted functions
   free of keeping an address? `std::function`'s and `std::thread`'s
   constructors are parameters that keep by RFC 0044's rule; are there
   others the table must mark?
7. **Placed locals in loops.** A slot per iteration is a pop and a push;
   is it worth hoisting the slot out of a loop whose body cannot keep it
   past an iteration?
8. **(D5) and mixed builds.** A debug unit linked with optimized ones
   mixes modes, which is allowed; does anything in a build system's
   caching make the default by `-O` a trap?
9. **Safe C's plain frames.** What RFC 0041's conversions need before
   their locals can leave tracked frames, since C has no `stale-borrow`
   for views of locals.
10. **Linux and libstdc++**: `_GLIBCXX_ASSERTIONS` on `std::span` and
    `std::string_view`, and thread-local access to the pool cache from
    compiled code under each TLS model.

## Future work

- **Iterators**: bounds carried by the iterator, or a rule for the
  explicit loop, and the end of (D6)'s exception; `m.find(k)->second`.
- **A second stack** behind plain frames.
- **Safe C's plain frames** (question 9).
- **Fix-its** for the notes that name one token (`WEAVEC_ESCAPES`,
  `WEAVEC_COUNTED_BY(0)`, RFC 0047's list), and a mode that applies
  them.
- **A large conversion**, on a project none of these RFCs has read.
- **The fast mode per function**, a run-time backstop for the thread
  rules, a checked cell: RFC 0046's and RFC 0047's lists, unchanged.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what the gates found. They override the body.

1. **Belief by type tag (§2.1).** An access through a pointer a fast C++
   unit's safe function loaded (from any memory, not only an annotated
   member), a project function returned (safe or not, directly or through
   a virtual call), or a parameter its entry checked, through phis and
   selects, has no guard when it is at a constant offset that the access's
   struct-path type tag (Clang's `!tbaa`) places in a member of the class
   the tag names, within the null page: the tag says the access is a
   member's, and typed memory says the pointer points to an object of its
   type. A vtable pointer's load at offset 0, and a load at a constant
   offset from a loaded vtable pointer (a virtual function's slot), are
   believed too. Every pointer and reference member of a fast unit's
   classes gets RFC 0042's field annotation (amendment 6 of RFC 0047
   annotated bounded members only), so loaded pointers are rooted with
   their pointee's size as in C. An object of a class with a virtual base
   is never sized from its type: its virtual base may lie outside it
   (yaml-cpp's `std::istream &`). A reference parameter and `this` are
   objects of their types for conversions (returning `this` as `T *`), but
   not roots: an access at a computed offset from them (a virtual base's)
   keeps its guard. Results of library functions are not believed by tag.
2. **Pointer results of project functions that are not safe (§7)** are
   believed with no range guard where they arrive, as RFC 0045 believes
   their references (Y-A1): the guard of a 128-byte node at every creation
   in tinyxml2's guarded arena cost as much as the conversion gained
   elsewhere.
3. **Three compile-time cases (§2.2), in C++ only** (Safe C's rules do not
   change): `call-args` for a position (a sum, a step, or a local that
   holds one, of a bounded pointer) returned, stored in a member, given to
   a constructor's member or passed for a project function's plain pointer
   parameter (the end a `WEAVEC_ENDED_BY` names is no parameter of this
   kind); `call-args` for a C library call that accesses, through a
   pointer the passes believe points to one object (a parameter, a member,
   a project function's result, `this`, `&x`), a count of bytes the table
   gives that is not a constant within the object (`min` of a constant and
   a string's length counts as the constant); and `index-single` for a
   read through a `WEAVEC_COUNTED_BY(0)` pointer. The second turned the
   conversions' first run-time failures (tinyxml2's `memcpy(..., xml,
   nBytes)` of a plain `const char *`) into compile errors.
4. **Strings (§3).** The checker marks no conversions: in a fast unit (C
   and C++) every constant step of a string position that no test covers
   tests the bytes it steps over, where it is made (each read at or before
   the terminator by the one before; `pointer-out-of-bounds` at the
   terminator), so the step is a position; steps up to 32 bytes (8
   before). A phi or select joins strings when its other inputs are null,
   undefined or literals, through chains of them (Clang's result slots). A
   string position needs no check to become a pointer to one `char`. A
   value of no known bounds converted to a string (a guarded function's
   result, a virtual call's) is checked in constant time when it lies in a
   live heap block, inline (the arena's range and the byte's shadow), and
   scanned by the runtime otherwise, at entries and everywhere else.
5. **Kept parameters (§4)** are found by a syntactic analysis of each
   function (`Kept` in `SafeCxx.cpp`) shared by the rule and the
   placement: what a pointer value points into (a parameter, `this`, a
   local, a temporary, through `&`, `std::addressof`, members, subscripts,
   locals that hold such a value) at the sinks the body names (an
   assignment to memory that is not a local, a member's initializer, an
   argument for a parameter that keeps). Besides `WEAVEC_ESCAPES` and
   aliased classes, a parameter keeps when it is a template's own
   parameter type (`T item`, a pointer by instantiation) of the program's
   templates, and a member of a library class template with a pointer
   type argument keeps its pointer arguments (`std::vector<T *>::
   push_back`). A by-value parameter is never placed: keeping its address
   is an error. An aliased class's `this` is not checked at entries (its
   locals are placed by type); an explicit `WEAVEC_ESCAPES` on `this` or a
   reference is.
6. **Placement (§5.1).** The checker adds the implicit `annotate("weavec.
   placed <type>")` to each placed local before the function's code is
   made (as each function is parsed, as an inline member is completed, and
   at the unit's end for deferred functions and instantiations), clearing
   NRVO so that a returned local is copied out; Clang emits it as
   `llvm.var.annotation`, which GuardInsert reads in every function of a
   C++ unit (guarded ones too, for aliased classes and for arguments to
   parameters that keep) before anything else. The type's name keys the
   pool. A temporary of an aliased class is an error only when a
   constructor makes one that nothing extends. A stale use of a placed
   local in the checked build reports `heap-use-after-free` with the
   block's allocation stack, not `stack-use-after-scope`: the report
   names the function that allocated it. The `stack-use-after-scope` lines
   of _Diagnostics_ were not built.
7. **Plain frames (§5.2)** as designed: an indirect call keeps a local
   tracked. Measured on tinyxml2, frames tracked across virtual calls cost
   nothing measurable.
8. **Views and range-based `for` (§6.1, §6.2) were not built.** The guards
   left in the two ports' and the two conversions' safe code are not
   there: they are in `std::sort`'s iterator code (D6) and in the
   conversions' guarded arenas. They stay _Future work_, with no table
   clause added.
9. **Library calls in hand (§6.4)** are the read-only calls whose pointer
   arguments the row reads as strings, each a string position or a
   literal; the call is marked and GuardExpand's late pass leaves it to
   the C library. A counted argument is not compared in hand; verify mode
   keeps the wrapper.
10. **Entries (§7).** A fast unit's inline safe functions are split too:
    the entry keeps the public `linkonce_odr` symbol (and comdat), the body
    is `.weavec.safe`, hidden and `linkonce_odr` in a comdat of its own, so
    a guarded unit's copy of the public symbol can win the link and the
    fast units' safe callers still call a body. A function with a
    `musttail` call is not split, and its `musttail` return is not
    converted. The `stack-escape` test of an escaping parameter runs at
    the entry only (a body's safe callers pass no stack address), inline
    when the address is not within 64 MiB above the stack pointer less 64
    KiB, and the runtime's test reads the thread's stack bounds once. A
    report therefore names the call that passed the stack address.
11. **Reference tests.** Fast safe C++ places none (O4 is the checked
    mode's); Safe C keeps RFC 0043's reference test in every mode, since
    §10 changes none of its rules.
12. **The allocator (§8)**: each pool reserves its own list of free slots
    (four million entries, mapped without reserve, made with the pool)
    where all pools shared 128 Ki entries each, and the pools may hold up
    to the most the heap held live rather than what it holds now. yaml-cpp
    no longer moves 2.8 million slots to the quarantine or sweeps; it was
    not faster for it, since the moves happened after its timed span. The
    inline paths were not built.
13. **The ledger (§9)** is version 11 with `placedLocals` in the summary
    (and `; <n> placed locals` on the summary line); no per-function
    `keeps` list and no `placed` list were added. The audit is version 7
    with `placedLocals`.
14. **The `-O0` default (§1, D5)** is the driver's: a command line with
    `-fweavec-safe` and no `-fweavec-safe-checks` gets the mode from its
    last `-O`, forwarded to each cc1 job; a hand-written cc1 line decides
    from its own optimization level.
15. **Line limit (D7):** the library is 27,600 lines; the limit is 27,600.
16. **What `Kept` follows (§4, §5.1), from G4.** A store into a view's
    member (a `WEAVEC_BORROWED` pointer) keeps nothing, as §4 says; a
    class with one is a view the rules of RFC 0044 follow. A
    constructor's member and base initializers are visited as its body
    is. A constructor keeps the object it makes when it says
    `WEAVEC_ESCAPES` or when its definition in the unit makes a member or
    base that is kept (a small buffer's own pointer, `_mem(_pool)`): a
    local so made is placed, in guarded code too; a temporary is
    `escape`; a safe constructor that makes one and does not say so is
    `escape`. A function inside a namespace is marked as the namespace
    closes, which is when its code is made. The analysis does not use
    Clang's parent map, which, built as the first function is parsed,
    misses the instantiations that follow (`raw-owner` then found no
    owner for a `new` in one). Without these, yaml-cpp's and tinyxml2's
    test suites stopped with `stack-escape` at entries their callers
    reached with a local that was not placed.
    From review: a member's default initializer is visited as the code
    of each constructor that does not name the member, and an implicit
    constructor whose default initializer points a member into its
    object keeps it; a local is placed by its class (a constructor of it
    that keeps its object, or an aliased class), so `auto k = Keeper(r)`
    and a factory's result are placed as `Keeper k(r)` is; a temporary a
    local reference extends is `escape`, since the mark names locals
    only. Of indirect calls, only a virtual call's result is believed
    (amendment 1), and the C library case of `call-args` is a function of
    C linkage or a system header's, not a project's of the same name.
17. **Diagnostics as built.** `escape` reads "the address of 'c' is kept
    by 'push_back', and 'add' only borrows it", with the note "declare the
    parameter WEAVEC_ESCAPES (after its '*' or '&') if 'add' keeps it; a
    caller's local then lives in typed memory"; a constructor's temporary
    is "a temporary is kept by its own constructor". The C library case
    of amendment 3 reads "'in' points to one object, and 'memcpy' may
    access more than its 1 byte through it". `stack-escape` keeps its
    report line (RFC 0041). G1's `ir-plain-frames.cpp` and
    `ir-library-in-hand.c` are cases of `ir-typed-cxx.cpp` and
    `ir-strings.c`; `rt_test`'s `placed-slots` and `pool-pairs` are
    `pool-keeps-many` and `string-entry` (the pair benchmark is not a
    test); `cxx-bounds.cpp` gained no case.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset) on
2026-11-15, with the compiler of the final commit, against main at
59a4bd71. The evidence is in `build/rfc48/` (not committed): `g3/` (the
seeded bugs, their twins, RFC 0046's sets), `g4/` (the two conversions,
their backups before the escape rounds and `tools/escapes.py`), `g5/` and
`g6/` (the builds and the timing scripts), `g9/`, `survey/`, `probe/` and
`alloc/`. The conversions were brought to a clean compile by the
implementer, with the pages, not by a fresh agent, and their time was not
recorded.

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass.** `test/Safe/cxx-kept.cpp`, `cxx-ends.cpp`; `test/Guards/placed-local.cpp`, `ir-typed-cxx.cpp` (plain frames and in-hand calls included), `ir-strings.c`, `stack-escape-entry.cpp`; `test/Driver/safe-checks-modes.cpp` (`-O0`); `rt_test` `string-entry`, `pool-keeps-many`; the ledger's and the audit's versions in `test/Driver/` (amendment 17 lists what moved). `ctest --preset dev` passes 50 of 50; the Linux container's `ci-release` (libstdc++, aarch64) 50 of 50. |
| G2 The stack | **Pass.** `stack-type.cpp` and `stack-ref.cpp` do not compile (`escape` at `push_back`). With `Counter &WEAVEC_ESCAPES c` the checked build stops with `heap-use-after-free` (amendment 6), the fast build prints the literal's address, and `verify` reports nothing. S0 (f)'s twelve programs were not written one by one; their cases (a reference, `this`, a temporary, a `thread_local`, a parameter taken by value, a constructor that keeps its object, a guarded caller) are in `cxx-kept.cpp`, `placed-local.cpp` and `stack-escape-entry.cpp`. |
| G3 Nothing believed is wrong | **Pass.** Built fast under `-fweavec-checks=verify`, no `weavec.proven` report: the 10 seeded bugs of RFC 0044 that compile as safe, tinyxml2's 517 checks, yaml-cpp's 1,008 tests, the LRU port's tests and the `json` port's 646 vectors (one fails, a lone surrogate, as it does in Clang's build). Built `full`, the 10 seeded bugs stop, as on main. Compiled: 83 of the 110 seeded bugs stop (83 on main); fixed twins rejected 23 (22), correct files accepted 84 of 90 (85), each new rejection one `WEAVEC_ESCAPES`; of RFC 0046's 160 programs one changes, `r45`'s fixed twin, rejected by `call-args` for a plain `const char *` given to `strncpy` with a count. |
| G4 The conversions convert again | **Pass.** **tinyxml2**: RFC 0047's conversion meets 19 errors (12 `call-args`, 7 `escape`); the converted copy has 415 of 444 functions safe (93.5%, 93.7% before) and passes its 517 checks in both builds with no report. Its string parameters were annotated in bulk (130 `WEAVEC_NULL_TERMINATED_OR_NULL` by one pattern), so its 175 added annotations overstate what the errors asked. **yaml-cpp**: 2,462 of 2,480 functions safe (99.3%, 99% before), its 1,008 tests pass in both builds and under verify with no report; 20 `WEAVEC_ESCAPES` and one `WEAVEC_BORROWED` in 16,218 lines, up four call chains. Both suites first stopped with `stack-escape` at entries, which amendment 16 fixed. |
| G5 Cost | **Fail.** Fast, times Clang's build of the same source (median of seven, interleaved; peak memory): tinyxml2 1.44 (1.72 before; 9.3 MiB against 3.2), yaml-cpp 1.58 (2.07; 401 MiB against 216), the `json` port 1.72 (1.73; 103 MiB against 88; Rust 0.94, 108 MiB), the LRU port 1.24 (1.18 in RFC 0045; 1.24 on this machine with the change switched off, `-mllvm -weavec-shadowless=0`; Rust 1.13). Checked: 1.79, 3.81, 2.87, 1.47. |
| G6 Safe C | **Pass.** User CPU against Clang's (min of five, interleaved), main then this branch: jsmn 1.26 and 1.26, sds 2.04 and 1.63, cJSON 2.20 and 1.99, inih 1.75 and 1.76, tinyexpr 1.17 and 1.17. |
| G7 Guards left | **Fail.** tinyxml2's fast ledger keeps 271 guards (376 before): 126 in safe functions (103 loop ranges, 18 `typed-unbounded`, 5 accesses), 58 in guarded functions, 87 in instantiations the ledger lists apart (its memory pools). The tenth asked is 38. |
| G8 The allocator | **Fail.** A typed pair costs 26.6 ns against the system allocator's 17.1 (1.56 times, batches of 20,000). yaml-cpp spends 178 ms in the runtime fast against 99 ms in the system allocator under Clang (1.80 times). The inline paths of §8 were not built (amendment 12). |
| G9 Nothing elsewhere | **Pass.** 3,624 compiles (RFC 0044's and RFC 0046's sets, `test/cases`, `test/Guards`; guarded, fast and checked). Guarded, 676 objects differ from main's: 668 only by the ABI number, and 8 are tests with safe functions or `WEAVEC_ESCAPES` (`placed-local.cpp`, `stack-escape-entry.cpp`, `cxx-bounds.cpp`, `safe-escape.c`, `safe-roots.c`, and three of `test/cases/safe/`). 8 compiles succeed with main's compiler only, the new rejections of G3. The documentation's examples pass. |
| G10 The survey again | **Recorded.** The four projects, unedited, errors before and after: leveldb 1,479 to 1,969 (`escape` 12 to 274, `call-args` 71 to 299), tinyxml2 195 to 244 (1 to 30, 0 to 20), pugixml 717 to 900 (9 to 164, 4 to 32), snappy 3 to 9 (0 to 2, 0 to 4). No other rule's count changes. |
| H1 Hygiene | **Pass.** Library 27,600 of 27,600 lines (amendment 15); the runtime 10,619 of 11,000. |

What the numbers say about the owner's goal: what a fast safe unit
believes rests on its rules, and three conversions and two ports show no
belief that was wrong. The checks it keeps are cheaper: tinyxml2, yaml-cpp
and Safe C's sds and cJSON are faster than on main, and nothing is
slower.
But this is not the cost of Rust. The conversions run at 1.4 to 1.7 times
Clang where the gates asked 1.25 to 1.4. What is left is not the guards
of safe code: it is the allocator (G8), the loops' range guards and the
guarded code around safe code (G7). The rules ask of existing code what
Rust's would: a constructor that keeps its argument, or points into its
own object, says so, and a conversion needs about one annotation per
thousand lines of yaml-cpp to say it.
