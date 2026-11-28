# RFC 0054: One of several types

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-22
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes, for safe functions,
  [RFC 0041](0041-safe-c.md)'s and [RFC 0044](0044-safe-cxx.md)'s rejection
  of a union member that holds a pointer where the union has a tag
  (`union-pointer`), and their rejection of a conversion from `void *`, from
  a base to a derived class and from a struct to a struct that begins with
  it (`pointer-cast`). Amends [RFC 0045](0045-type-stable-memory.md) §2 (an
  allocation's type is recorded in every build, not only the fast one) and
  [RFC 0048](0048-safe-code-without-the-shadow.md) §4 (a local whose address
  leaves its function has a record of its type). `std::variant`,
  `dynamic_cast` and the borrow rules are unchanged.

## Summary

A program holds values that are one of several types: a JSON value, a
syntax tree's node, a message, the context a callback is handed. Rust
writes them as an `enum` and a `match`, a trait object, a `dyn Any`; an
agent writing Rust uses them on every page. Safe C has no form for them at
all, and Safe C++ has `std::variant` and `dynamic_cast` only. A C union
with a pointer in it is an error with no fix (`union-pointer`), and so is a
`void *` converted back to what it points to and a pointer to a base
converted to the object it is part of (`pointer-cast`). On eleven C
projects those are 2,867 of 8,183 rule errors, the largest group after the
one annotations answer; on the C++ interpreter an agent wrote with no
checker in mind they are 41 of 79, and the rewrite the rule asks for costs
1.22 to 1.35 times the run time.

This RFC makes the three forms checked forms, each checked where the type
is chosen and by nothing else:

- **A tagged union.** A union in a struct says which field is its tag
  (`WEAVEC_TAGGED_BY(kind)`) and each member which values of the tag it is
  for (`WEAVEC_WHEN(KIND_TEXT)`). A safe function reads or writes a member
  only after a comparison of the tag, which the compiler adds; a store of
  the tag clears the union; and the address of a member is not taken, so
  no pointer into a member outlives the comparison.
- **A conversion from `void *`.** `struct node *n = context;` compiles in a
  safe function as a question to the runtime: is there a `struct node` at
  this address? The runtime knows, because every block, global and local
  whose address leaves its function now has a record of its type.
- **A downcast.** `static_cast<Circle &>(shape)`, C's `(struct circle *)s`
  where a `struct circle` begins with a `struct shape`, and `container_of`:
  the same question. For a class with virtual functions it is one
  comparison of the object's vtable pointer.

A wrong answer stops the program with a report (`union-member`,
`bad-cast`) in both builds. Nothing here rests on the borrow rules, on
reach or on what a call may change: each check reads the tag or the type
at the moment of use. The annotations are two; the runtime's ABI, the
ledger and `weavec.h` change.

## Motivation

### What the rules cannot say today

Compiled with the rules only (`-fweavec-safe-survey`), eleven C projects
of the corpus as they are written give 8,183 distinct errors
(`build/rfc54/s0/survey/`: cJSON, hiredis, http-parser, inih, jansson,
libyaml, linenoise, Lua, mujs, sds, wren):

| Rule | Errors | Share | What answers it today |
| --- | ---: | ---: | --- |
| `index-single` | 3,306 | 40% | a bound (`WEAVEC_COUNTED_BY`), which `--weavec-fix` proposes |
| `union-pointer` | 1,853 | 23% | nothing: "one pointer per variant in a struct" |
| `pointer-cast` | 1,014 | 12% | nothing: "a `WEAVEC_GUARDED` adapter" |
| `variadic`, `no-asm` | 826 | 10% | a guarded function |
| `escape`, `call-args`, `field-address` | 782 | 10% | annotations |
| the rest | 402 | 5% | |

7,069 of them carry no fix. The two rows this RFC is about differ from the
others in kind: an `index-single` error is a fact the code already has and
has not said, and saying it clears the error. A `union-pointer` or
`pointer-cast` error is a design the rules have no word for. The page for
the first says to keep every alternative in a struct side by side; the
page for the second says to leave the function guarded. Neither is what an
agent told to make a program safe should write, and RFC 0049's two-hour
conversions show what follows: nng reached 61% of its functions safe with
632 guarded adapters whose body is a conversion from `void *` and a call.

What the errors are:

- **`union-pointer`** is Lua's `TValue` (1,222), mujs's `js_Value` (438),
  libyaml's events and tokens (168), hiredis (16) and wren (9). Every one
  of those unions sits in a struct beside a field that says which member is
  live, and the code tests that field before it reads a member. The check
  is in the program; the rules cannot see that it is the check.
- **`pointer-cast`**: 534 are a pointer to a struct converted to a pointer
  to another, in both directions: wren's `Obj *` and `ObjString *` (235; an
  `ObjString` begins with an `Obj`) and Lua's `GCObject *` and `Table *`
  (299; both begin with the fields of one header macro). 247 are a `void *`
  converted to a struct pointer (a callback's context, an allocator's
  result, a container's element), 14 of them to a pointer to a pointer;
  133 are a `void *` taken as bytes; the other 100 are conversions that
  should stay errors (`T **` to `void **`, bytes to a struct that holds
  pointers, function pointers).

New code has the same shape. RFC 0051's trial interpreter, written by an
agent as natural C++20, has 79 rule errors, and 41 are `pointer-cast`: a
`static_cast` to the node's class after a `switch` on its `kind`. The agent
that made it safe replaced them with `dynamic_cast`. Measured again today
with Clang (`build/rfc54/s0/cast/`, medians, interleaved):

| The interpreter's benchmark | Time | Ratio |
| --- | ---: | ---: |
| as written (`static_cast`) | 1,064 ms | 1.00 |
| each cast a `dynamic_cast` | 1,437 ms | 1.22 to 1.35 |
| each cast one comparison of the object's type | | 0.98 |
| made safe, Clang | 1,502 ms | 1.41 |
| made safe, the fast build | 2,181 ms | 2.05 |

The third row is the 41 casts written as `&typeid(e) == &typeid(T)` and a
`static_cast`, with all 37 tests passing: a load of the vtable pointer, a
load of its type and a comparison, which costs nothing that can be
measured here. A checked cast is free where `dynamic_cast` is a third of
the program's time. The fast build of the safe program is 2.05 times the
program as written; RFC 0051's _Future work_ named checked casts as the
first of the two designs that number asks for.

### Why this, of what is left

RFC 0050 left the fast build at 1.22 to 1.52 times Clang on safe sources,
and its record has Rust's port of the `json` brief at 433 ms where the two
C++ ports run 633 ms and 832 ms in the fast build. RFC 0052 and RFC 0053
left twelve known ways to keep a stale borrow. Both matter, and neither is
where an agent's rewrite fails first. A rewrite fails first where the
rules have no accepted form for what the program is: a value of one of
several types. In C that is every interpreter, parser, protocol and
container; in C++ it is every class hierarchy walked by kind. Rust's
answer is the feature agents lean on most. WeaveC's answer has been an
error with no fix in C and a slower program in C++.

It is also the cheapest of the three to get right. A tag comparison or a
type comparison at the use is complete by construction: there is no path
through a call, a callable or a container for a rule to miss, because the
check does not ask how the value got there.

### What the last round's holes were

One measurement belongs here because it says what this RFC does not do.
Of the 74 holes of RFC 0053's fresh round (`build/rfc53/g4`), 71 are one
mechanism: a reference into the alternative of a
`std::variant<std::string, long>`, used after the variant holds the other
alternative. Both builds and AddressSanitizer are silent, since the object
is alive. The other three are heap blocks used after release, which the
checked build stops and the fast build reads as their own type, as RFC
0045 designs it. So what rests on the borrow rules alone is narrower than
(X3) reads: storage that changes type in place, and a reference into it
that is held across the change. Tagged unions as §3 defines them cannot
have that hole, because no reference into a member exists. `std::variant`
can, and this RFC leaves it to the rules; _Future work_ says what a check
at run time for it would need.

## Soundness

### What is guaranteed that was not

For a safe function, in both builds, under RFC 0041's and RFC 0044's
assumptions:

- **(S1) A member of a tagged union is used only while the tag names it.**
  Each read or write of a member, or of anything inside it, follows a
  comparison of the tag with the member's values in the same expression.
- **(S2) A tagged union holds the member its tag names, or zeroes.** A
  store of a different value to the tag by a safe function clears the
  union's bytes, and a zero is a value of every type a member may have: a
  null pointer, a zero count, an empty string's first byte.
- **(S3) No pointer into a member outlives its comparison.** A safe
  function does not take the address of a member of a tagged union that
  holds a pointer or shares its bytes with one, and does not copy the
  union apart from its tag.
- **(S4) A checked conversion gives a pointer to an object of its type.**
  A pointer made from a `void *`, from a base or from a struct's first
  member points at memory that holds an object of the target type, live or
  released, as type-stable memory has it (RFC 0045 (Y1)), or the
  conversion stops with `bad-cast`. Null converts to null.

### What it rests on

- **The records are right.** A block's type is the type Clang infers at
  its allocation (RFC 0045 §2), which Safe C's `pointer-cast` already
  requires the allocation to state. A global's and a local's are their
  declarations'.
- **Code that is not safe keeps the types** (X-A1, unchanged): it stores a
  tag and a member that agree, and it hands safe code a `void *` to what
  the safe code will take it for. Memory that has no record (a block of
  another allocator, a frame of code another compiler built) is believed,
  as a pointer a fast unit receives is (RFC 0048 §1), and the ledger and
  `WEAVEC_RT_STATS` count the conversions that were believed.
- **One thread at a time for a tagged union in C.** Safe C has no thread
  rules: a tag another thread stores between the comparison and the use is
  not seen. Safe C++'s thread rules apply to a struct with a tagged union
  as to any struct.

### Bugs caught

```c
struct value {
  enum kind kind;
  union {
    double number WEAVEC_WHEN(KIND_NUMBER);
    char *WEAVEC_NULL_TERMINATED text WEAVEC_WHEN(KIND_TEXT);
  } as WEAVEC_TAGGED_BY(kind);
};
char first(const struct value *v) { return v->as.text[0]; }
// first(&(struct value){.kind = KIND_NUMBER, .as.number = 3.0})
// stops: union-member, 'text' read while 'kind' is KIND_NUMBER (0)
```

```c
struct circle { struct shape base; double radius; };
double radius(struct shape *s) { return ((struct circle *)s)->radius; }
// radius(&square->base) stops: bad-cast, a 'struct circle' is not at s;
// it is the start of a block of type 'struct square'
```

```c
void on_timer(void *context) {
  struct server *server = context;   // checked: is a 'struct server' there?
  server->ticks++;
}
```

```cpp
double area(const Shape &shape) {
  switch (shape.kind) {
  case Kind::Circle: return static_cast<const Circle &>(shape).area();
  ...                 // one comparison; a Square tagged Circle stops
```

### Bugs deliberately not caught

- A reference into the alternative of a `std::variant` held across a
  change of alternative: the borrow rules, as before.
- A tag or a `void *` that code that is not safe got wrong before any safe
  function checked it, and what such code does with a union.
- An integer made into a pointer (`int-to-pointer`, wren's and Lua's
  packed values), a function pointer converted, and a conversion between
  unrelated structs: still errors.
- A stale pointer converted: a released block keeps its type, so the
  conversion passes and the pointer is as stale as it was (RFC 0045).

### Accepted rejections and stops

- **A member's address.** `&v->as.text`, a member array passed where it
  decays to a pointer that can write, and a C++ reference bound to a member
  are errors. Copy the member out, or pass the struct. A member that holds
  no pointer may be passed as a pointer to `const`.
- **The union apart from its tag.** `union payload p = v->as;` is an
  error; copy the struct. Lua's `setobj`, which copies the union and then
  the tag, is rewritten as one struct assignment.
- **A member stored before its tag.** `v->as.text = s; v->kind =
  KIND_TEXT;` would stop at the first statement. Where both stores are in
  one block the compiler rejects it and the certain fix sets the tag
  first.
- **Correct code that stops:** none is expected. A conversion of a pointer
  into memory with no record is believed, not stopped (D5).

## Detailed design

### 1. Type records

The runtime answers one question: **is an object of type `T` at this
address?** It needs, for each object, its type, and for each type, what it
is made of.

**1.1 A type's layout.** RFC 0045's `struct __weavec_rt_type` (a key
hashed from the type's name, and a cache) gains a pointer to a layout the
compiler emits once per type per unit (`linkonce_odr`):

```c
struct __weavec_rt_layout {
  uint64_t key;      /* the type's key (RFC 0045 section 2) */
  uint64_t size;     /* sizeof */
  int64_t name;      /* offset to its name, for reports */
  uint32_t flags;    /* WeavecRtLayoutPlain: no pointer, no bool */
  uint32_t count;    /* parts, sorted by offset and key */
  struct {
    uint32_t offset; /* of the first element */
    uint32_t stride; /* element size; 0 for one object */
    uint32_t elements;
    uint32_t flags;  /* WeavecRtPartUnion: inside a union */
    uint64_t key;
  } parts[];
};
```

The parts are every base, member and array element of a struct, class or
union type inside the type, at any depth, flattened: a `struct circle` has
a part for its `struct shape` at offset 0. Scalars have no part. A part
inside a union is marked: no conversion finds an object there, since only
the tag says whether it is one.

**1.2 Where an object's type is kept.**

| Object | Record |
| --- | --- |
| A heap block | the slot's type, in every build. The fast build has it as the slot's pool (`slotPools`, RFC 0045 §3). The checked build and guarded units now pass the type Clang infers to the allocator as well, which records it and does nothing else with it: the block is still released through the quarantine. |
| A global | `struct __weavec_rt_global` gains its layout. |
| A local in a guarded frame | the frame's object record (RFC 0040 §5.3) gains its layout. |
| A placed local (RFC 0048 §4) | its block's, as a heap block. |
| Another local of a safe function whose address leaves the function | a **frame record**: its address and layout on a list the thread keeps, pushed where its scope begins and popped where it ends. The local stays in its plain frame. An entry below the stack pointer is dead and is dropped when it is met. |

The address of a local leaves its function when it is passed to a function
of the program, stored, returned or converted to `void *`. A local passed
only to functions of the library table needs no record: they convert
nothing.

**1.3 The question.** `__weavec_rt_type_at(address, type)` finds the
object that holds the address (the slot, the global, the frame object, the
frame record), takes the offset into it, reduces it by the element size
for a block that holds an array, and looks for a part with that offset (or
that offset within its stride) and the type's key; offset 0 with the
object's own key is the common case and is tried first. A block whose type
is `WeavecRtLayoutPlain` also answers yes for any plain scalar type that
fits. The answer for memory with no record is "believed" (D5), and a heap
block that has no type takes the type of the first conversion that asks,
when it fits, as C's effective type rule has it; later conversions must
agree.

A released block answers as it did before its release: the fast build's
slot keeps its pool, and the checked build's keeps its type until the
sweep recycles it (the access that follows a stale conversion is the
guard's to report).

**1.4 Cost.** A record is 4 bytes a slot, which the fast build already
spends. S0 measures the call and decides whether a conversion gets an
inline path (the slot's type compared with the one the site last matched,
as a typed allocation caches its pool).

### 2. Checked conversions

**2.1 Which conversions.** In a safe function these stop being
`pointer-cast` errors and are compiled as checks:

- (a) `void *` to `T *`, `T` a struct, class or union type: C's implicit
  conversion, a C cast, C++'s `static_cast`.
- (b) `B *` to `D *` (and `B &` to `D &`) where `B` is a base class of `D`
  in C++, or in C the type of `D`'s first member, or of its first member's
  first member.
- (c) C's `container_of`: a cast to `D *` of a byte pointer made from an
  `M *`, less `offsetof(D, m)` where `m` is an `M`. (C++'s `static_cast`
  applies a base's offset itself.)
- (d) `void *` to a pointer to a scalar without a pointer or a `bool`
  (`unsigned char *`, `int *`, `double *`): the object must be plain.

One conversion needs no check and stops being an error: `D *` to `B *`
where `B` is the type of `D`'s first member (`(Obj *)string` is
`&string->obj`).

These stay errors: a struct to an unrelated struct, bytes to a type that
holds a pointer, `T **` to `void **`, function pointers, integers,
`reinterpret_cast` beyond RFC 0047's byte views, `const_cast`.

What a conversion gives is a pointer to one object, with the ownership and
the bound its declaration states, as any pointer the function is handed.
An array's elements convert one at a time (`const struct item *a = left;`
in a comparator `qsort` calls).

**2.2 The check.**

- For a class with virtual functions: the object's vtable pointer is
  compared with `D`'s. Equal is the answer for an object that is exactly a
  `D`, which a leaf class's always is. Otherwise the runtime asks the C++
  ABI what `dynamic_cast` asks (`__weavec_rt_cast_class`), and the result
  must be the pointer the `static_cast` computed. Null passes for a
  pointer. In a unit without run-time type information the records of §1
  answer.
- For every other type: `__weavec_rt_type_at(result, D)`, on the pointer
  after any offset is applied.
- Failure reports `bad-cast` and stops (`-fweavec-checks=report` goes on,
  as for any report).

The check is made in both builds and in a safe function only. A guarded
function's conversions are trusted, as its bounds are.

**2.3 How the pass finds a conversion.** A conversion between pointers
leaves nothing in the IR. The checker, which already changes the AST
before code is made (it adds `weavec.placed`, RFC 0048), wraps the operand
of each conversion of §2.1 in a call of a marker (`__weavec.cast`, with
the target's type record), which `GuardSafe` expands into the check; and
likewise the base of each member access of §3 (`__weavec.member`). For
polymorphic downcasts S0 also tries the sites of Clang's own check
(`-fsanitize=vptr`'s downcast kind, with the other kinds removed) and
keeps whichever is smaller (_Unresolved question_ 1).

### 3. Tagged unions

**3.1 The declaration.**

```c
struct value {
  enum kind kind;
  union {
    double number WEAVEC_WHEN(KIND_NUMBER);
    bool truth WEAVEC_WHEN(KIND_TRUE, KIND_FALSE);
    char *WEAVEC_OWNED WEAVEC_NULL_TERMINATED text WEAVEC_WHEN(KIND_TEXT);
    struct list *WEAVEC_OWNED items WEAVEC_WHEN(KIND_LIST);
  } as WEAVEC_TAGGED_BY(kind);
};
```

- `WEAVEC_TAGGED_BY(tag)` on a field of union type (or after the closing
  brace of an anonymous union) names a field of integer or enumeration
  type of the same struct. `WEAVEC_TAGGED_BY(tag, mask)` compares the tag
  after `& mask` (Lua keeps other bits in its tag).
- `WEAVEC_WHEN(v, …)` on a member lists the constant values of the tag the
  member is for. A member without one is for no value and is never read in
  a safe function.
- Two members may share a value only if neither holds a pointer or a
  `bool`, or they have the same type. A member that holds a pointer in a
  tagged union must have a `WEAVEC_WHEN` (`bad-annotation` otherwise).
- A union type declared on its own (`union Value`) carries the
  `WEAVEC_WHEN`s, and each field of that type its own `WEAVEC_TAGGED_BY`.
  An object of the union type that is not such a field is not tagged, and
  `union-pointer` applies to it as today.
- In C++ the same holds for a union whose members have trivial
  destructors. A union with a member that has a destructor stays an error;
  `std::variant` is its form.

**3.2 The rules** (`union-pointer`, in the function that breaks them):

- A member is read or written through its struct: `v->as.text`,
  `v->as.pair.first`, `v->as.small[i]`.
- The address of a member is not taken (S3), with the exception for a
  pointer to `const` of a member that holds no pointer.
- The union is not copied, passed or returned apart from its struct.
- A member is not stored before a store of its tag in the same block (the
  certain fix moves the tag's store first).
- An initializer or a compound literal sets the tag and the member
  together, and `{0}` is the member of tag 0.

**3.3 What is compiled.**

- **A member access** is preceded by a load of the tag and a comparison
  with the member's values (one value: an equality; several: a range or a
  bit test). The failing branch calls `__weavec_rt_union_fail` with the
  site and the tag: `union-member`. The optimizer removes the comparison
  the program already made in the same function when nothing between them
  can store the tag.
- **A store of the tag** by a safe function loads the old value first; if
  the new one differs, the union's bytes are set to zero after the store.
  A struct assignment and an initializer write the tag and the union
  together and clear nothing.
- Nothing else changes: the struct's layout is the one it had, so code
  that is not safe and other compilers see the same type.

**3.4 Owners.** A store of the tag counts as a store of null to each
`WEAVEC_OWNED` member of the union (RFC 0043's rule for a store over an
owner field): release or move the member first, or the store is
`owner-leak`. A member's pointer fields are typed memory as any field's
(RFC 0042): their bounds are their declarations'.

### 4. Fixes the errors carry (RFC 0049)

- **E9, a tag from the tests the code makes.** For a `union-pointer` error
  on a union in a struct, the planner looks at every read of a member in
  the unit: if each is dominated by a `switch` on, or a comparison with a
  constant of, one integer field of the same struct, the fix (likely)
  writes `WEAVEC_TAGGED_BY` on the union and a `WEAVEC_WHEN` on each member
  with the values its reads were under. A member read under no test, or
  under two fields, makes it a choice the note prints.
- **The tag first** (certain), as §3.2.
- `pointer-cast` errors of the forms of §2.1 are no longer errors and need
  no fix. A guarded function whose body is such a conversion and a call
  (nng's adapters) is listed by the audit as one that can be safe.

### 5. What changes elsewhere

- **The rules' pages** (`resources/safe/rules.md`): `union-pointer` and
  `pointer-cast` are rewritten around the checked forms; the advice to
  write a guarded adapter and to keep one pointer per variant goes. The
  model page gains "one of several types". `dynamic_cast` stays accepted.
- **The guides**: Safe C gains "A value of one of several types" and
  "Contexts and containers of `void *`"; Safe C++'s building block for a
  hierarchy uses `static_cast` after a test of the kind; "Coming from
  Rust" gains `enum` with `match` (C: a tagged union and a `switch`),
  `dyn Any` (`void *`) and a trait object downcast.
- **The guarantees page**: (S1) to (S4); (W) and (X) lose the two
  rejections.
- **RFC 0045 §2**: every build passes an allocation's type to the
  allocator. The fast and the checked build no longer differ in whether a
  block's type is known, only in what the allocator does with it.
- **The ledger** (version 14) records each checked conversion (`cast`,
  with its form) and each tagged union's accesses (`member`), and the
  summary line counts them. The audit (version 10) lists guarded functions
  that only convert and call, and the units whose conversions were
  believed at run time when run with `WEAVEC_RT_STATS`.
- **The runtime** (ABI 49): `__weavec_rt_type_at`, `__weavec_rt_cast_class`,
  `__weavec_rt_cast_fail`, `__weavec_rt_union_fail`, the frame records'
  list, the layout in a type, a global and a frame object.
- **`weavec.h`** 3.5 (see _Annotation surface_).

### 6. What does not change

`std::variant` and what the borrow rules say of it; `dynamic_cast`;
RFC 0047's byte views; `int-to-pointer`; guarded and unsafe functions,
whose conversions and unions compile as before; a program with no union,
no `void *` conversion and no downcast, whose safe code is the same but
for the frame records of §1.2.

### 7. Stages and gates

- **S0. Measure first.** (a) The marker of §2.3 through the pipeline on
  one conversion and one member access. (b) The 41 casts of the
  interpreter as written, checked by the compiler: at most 1.02 of
  `static_cast` under the fast build. (c) A small bytecode interpreter in
  C over a tagged value, its member reads checked: at most 1.05 of the
  same program unchecked. (d) `__weavec_rt_type_at` on a heap block: at
  most 25 ns, or the inline path of §1.4. (e) Frame records on RFC 0050's
  programs (the two json ports, the LRU port, tinyxml2, yaml-cpp): at most
  1.01 of main's fast build. A limit missed goes back to the owner with
  the numbers (D8).
- **S1.** The runtime: layouts, the type of every block in every build,
  globals, frame objects, frame records, `__weavec_rt_type_at`, with
  `rt_test` tests.
- **S2.** Checked conversions in C: `void *`, first members,
  `container_of`, plain scalars.
- **S3.** Checked downcasts in C++.
- **S4.** Tagged unions: the annotations, the rules, the comparisons, the
  cleared union, owners.
- **S5.** E9 and the certain fix, the ledger and the audit, the rules'
  pages, the guides, the guarantees page, the roadmap.

Gates:

- **G1. Tests.** Each mechanism is pinned: `test/Safe/tagged-union.c`,
  `checked-cast.c`, `cxx-checked-cast.cpp`, `fix-tagged.c`;
  `test/Guards/union-member.c`, `bad-cast.c`, `bad-cast.cpp`,
  `ir-tagged.c`, `ir-cast.c`, `ir-cast.cpp`; cases `test/cases/sum/`;
  `rt_test` `type-at-*`, `frame-records`.
- **G2. The survey.** On the eleven projects, with no edit, at least 600
  of the 1,014 `pointer-cast` errors are gone (wren's 235, the 233 from
  `void *` to a struct, the 133 to bytes; Lua's 299 wait on _Unresolved
  question_ 4). After `--weavec-fix`, at least half of the 1,853
  `union-pointer` errors are gone, and each one left is listed with its
  reason.
- **G3. The interpreter as written.** It has no `pointer-cast` error
  (38 errors or fewer of the 79). A fresh agent makes it safe with its
  casts kept, in no more than RFC 0053's 4 minutes times 1.5, and the fast
  build runs the benchmark in at most 1.70 times the program as written
  (2.05 today).
- **G4. New C.** A fresh agent given RFC 0044's `json` brief writes the
  port in Safe C: every function safe but those that read and write files,
  no guarded function for a conversion or a union, `check.py` ends with
  `0 failed` in both builds. Its minutes, rounds and benchmark are
  reported beside the C++ and the Rust ports'; the benchmark is at most
  1.5 times Rust's.
- **G5. A conversion.** On nng as RFC 0049's G5 left it, an agent with one
  hour removes the adapters this RFC makes unnecessary: at least 70% of
  functions safe (61% today), tests passing in both builds.
- **G6. A round.** Two fresh adversarial agents, one for C and one for
  C++, try to forge a pointer in all-safe code through a tagged union or a
  checked conversion. Every hole is rerun. The gate is none.
- **G7. Cost elsewhere.** RFC 0050's programs within 1.01 of main's fast
  build and 1.02 of its checked build; a guarded program (the corpus gate's
  quick tier) within 1.02.
- **G8. Compile time** within 1.02 of main's, with a Release compiler.
- **G9. The lines of code** the library and the runtime gain, reported.
  The draft expects about 1,600 in the library and 500 in the runtime.

## Annotation surface

Two macros in `resources/include/weavec.h` (3.5), with their constants in
`include/weavec/Frontend/SafeCheck.h` (`TaggedByAnnotation`,
`WhenAnnotation`) and their rows in `docs/annotations.md`:

| Macro | Attaches to | Expands to | Effect |
| --- | --- | --- | --- |
| `WEAVEC_TAGGED_BY(tag)`, `WEAVEC_TAGGED_BY(tag, mask)` | a field of union type, or an anonymous union | `__attribute__((annotate("weavec.tagged_by", "tag", mask)))` | the union's members are read and written under a comparison of `tag`; a store of `tag` clears the union |
| `WEAVEC_WHEN(value, …)` | a member of a union | `__attribute__((annotate("weavec.when", value, …)))` | the values of the tag the member is for |

Both expand to nothing under a compiler that is not WeaveC, and neither
changes a type's layout.

## Diagnostics

Compile time, under existing rules, each pinned in `test/Safe/` with its
note:

- `union-pointer`: `reading union member '<m>' may reinterpret another
  member's bytes as a pointer` (as today), with the note `say which field
  tells which member is live: 'WEAVEC_TAGGED_BY(<tag>)' on '<union>' and
  'WEAVEC_WHEN(…)' on its members` and E9's fix where there is one.
- `union-pointer`: `the address of '<m>' would outlive the check of
  '<tag>'`; note: `copy '<m>' out, or pass '<struct>'`.
- `union-pointer`: `'<union>' is copied without '<tag>'`.
- `union-pointer`: `'<m>' is stored before '<tag>' is set to '<value>',
  and the store of '<tag>' clears it`; note: `fix: set '<tag>' first`.
- `owner-leak`: `the store of '<tag>' clears '<m>', which still owns its
  block`.
- `bad-annotation`: `'WEAVEC_TAGGED_BY(<tag>)' names no integer field of
  '<struct>'`; `'<m>' holds a pointer and has no 'WEAVEC_WHEN'`; `'<a>' and
  '<b>' are both for '<value>' and one holds a pointer`.
- `pointer-cast` keeps its messages for the conversions of §2.1's last
  paragraph.

Run time, two new kinds, each with a `test/Guards/` test that pins its
line, an entry in `docs/pages/reference/cli.md` and in
`scripts/run-cases.py`:

- `union-member`:
  `weavec: union-member at <file>:<line>:<col>: '<member>' <read|written> while '<tag>' is <value>`

  ```c
  struct value v = {.kind = KIND_NUMBER, .as.number = 1.0};
  return v.as.text[0];
  ```

- `bad-cast`:
  `weavec: bad-cast at <file>:<line>:<col>: <address> is not a '<type>'`,
  followed by the object's paragraph as RFC 0040 has it, which now names
  the object's type (`<n> bytes into a block of <size> bytes of type
  '<type>' allocated at …`, a global, a local of a frame).

  ```c
  struct shape *s = &square->base;
  return ((struct circle *)s)->radius;
  ```

## Drawbacks

- **Two more annotations**, and a union's members now carry one each.
  E9 writes most of them; a union with no tag beside it still has no
  form.
- **A record for every local whose address leaves a safe function.** Two
  stores where its scope begins and one where it ends, for conversions
  most programs never make of it. S0 (e) limits it to 1%.
- **Type layouts in every object file**, one per type that is allocated,
  global or recorded.
- **Believed memory.** A conversion of a pointer into memory with no
  record passes. It is what a fast unit already does with every pointer it
  receives, and it is counted, but (S4) is as strong as the records are
  complete.
- **Tagged unions are stricter than C.** No member's address, no union
  apart from its tag, the tag before the member. Lua's macros do not
  compile as they are.
- **The checker changes the AST** for two more forms (§2.3).
- **The first conversion types an untyped block**, so a wrong first
  conversion makes the right one stop later.

## Alternatives

- **Do nothing; keep the adapters and `dynamic_cast`.** The measurements
  above are its cost: a third of C's errors with no fix, and 1.22 to 1.35
  on a hierarchy walked by kind.
- **A check at run time for `std::variant`'s alternatives first**, since
  71 of the last round's 74 holes are borrows into one. It is the larger
  soundness prize and the harder design: a reference into an alternative
  is a plain reference once a callee has it, so the check must be at the
  change (the variant held while an alternative is lent, RefCell's flag),
  with a way to carry the hold to a caller a reference is returned to, and
  trivially copyable variants change alternative with no call to hook. It
  needs a prototype before a draft. This RFC's records are what it would
  be built on.
- **Lay a union's members side by side** (a product), so that no bytes
  change type. The size is the sum of the alternatives, where RFC 0051
  measured 1.13 for one such type made by hand; and Clang's code for a
  union member applies no offset whatever layout an external source gives
  (as this draft reads `EmitLValueForField`; not tested), so it would need
  a change to Clang.
- **A hidden tag**, as Cyclone's `@tagged` unions have. It changes the
  struct's layout, so the type no longer matches the headers other code
  was built with; the programs measured all have a tag already.
- **Check the tag where a member's address is used**, to allow the
  address. That is the borrow problem again, in C, where there are no
  borrow rules for it.
- **`dynamic_cast` compiled as a comparison.** Clang does it for a final
  class under `-fassume-unique-vtables`, which did not change the
  measurement above for casts to references, and it leaves C, `void *` and
  classes without virtual functions where they are.
- **Clang's CFI cast checks.** They need link-time optimization and hidden
  visibility for the whole program.
- **A type check on every access** (EffectiveSan). Complete, and several
  times the cost of the guards the fast build removed.
- **Stop on memory with no record.** Sound, and a false stop for every
  callback a library built by another compiler hands its own block to.

## Prior art

- **Cyclone**: `@tagged` unions with a hidden tag checked at each read,
  and its rule that no pointer into a member is made. This RFC takes the
  rule and keeps the program's own tag.
- **CCured**: run-time type information for the pointers its inference
  could not type (`RTTI` pointers), exactly for downcasts and `void *`. It
  carried the type in the pointer; WeaveC keeps it with the object, which
  type-stable memory makes sufficient.
- **EffectiveSan** (Duck and Yap): an object's type and a layout table
  found from the address, checked at every use. The layout of §1.1 is its
  table; the check is made only where a type is chosen.
- **HexType, TypeSan, UBSan's `vptr`, LLVM CFI's derived-cast check**:
  bad C++ casts found at the cast. §2.2's first step is their fast path.
- **Rust**: `enum` and `match`, `Any::downcast_ref` (a comparison of type
  ids), and no way to reach a payload but through the comparison.
- **Checked C and `-fbounds-safety`**: neither gives unions or `void *` a
  checked form; both leave them to unchecked scopes, as WeaveC did.
- **`kalloc_type`, typed `xzone` malloc, LLVM's allocation tokens**: the
  type of a block known to the allocator, which RFC 0045 built on and this
  RFC reads back.

## Unresolved questions

1. **How the pass finds a conversion and a member access** (§2.3): the
   marker call the checker adds, Clang's own downcast check sites, or keys
   by location as RFC 0050's range-based `for` uses. S0 (a) decides.
2. **The inline path** of a `void *` conversion (§1.4): S0 (d).
3. **Frame records for which locals.** Every local whose address leaves
   its function, or only structs, unions, classes and arrays of them? A
   scalar local's address converted to a struct pointer through a `void *`
   is the case the wider answer closes. S0 (e) measures both.
4. **Structs that begin alike.** Lua's `Table` and `GCObject` share their
   first fields through a macro, not a member, so neither is a part of the
   other in §1.1's layout: a `Table *` taken as a `GCObject *` reads only
   those fields, and the conversion back is answered by the block's own
   type, but a `void *` to a `Table` asked for as a `GCObject` finds none.
   Should C's common initial sequence count as "begins with" (the compiler
   adding a part for each struct of the unit that is a prefix of another),
   or should such headers be written as a member? S2 measures Lua's 299
   errors under each. Related: a tag that is in an object's header, not
   beside a union (wren's `Obj`), is answered by §2 and not by §3; G2's
   list of the errors that remain will say whether anything is left
   between the two.
5. **Tag values by expression.** A mask is in; is a predicate
   (`WEAVEC_WHEN(kind >= KIND_FIRST_OBJECT)`) needed? Deferred until a
   program asks.
6. **Safe C and threads.** A tag stored by another thread is outside
   (S1). Safe C has no thread rules at all, which a later RFC should own.

## Future work

- **Alternatives that are held**: the check at run time for a borrow into
  a `std::variant`, a `std::expected`, a `std::any` and a `std::function`
  across a change of what they hold, as _Alternatives_ sketches, with the
  gate that a fresh round finds no hole in either build. With it, a
  `WEAVEC_ALIASED` class may own a variant (RFC 0051's "sum types in
  shared objects").
- **Integers that are pointers** (`int-to-pointer`): wren's and Lua's
  packed values, as a conversion checked by `__weavec_rt_type_at`, once the
  sweep can be told where they are.
- **Bounds a block knows**: `index-single`, 40% of the survey, where the
  bound is the block's own and the record of §1 could give it.
- **`std::any` and type-erased owners** read through the same records.
- **Thread rules for Safe C.**

## Decisions to accept

Accepted as drafted on 2026-11-22.

- **D1. Scope.** Tagged unions and checked conversions, in Safe C and Safe
  C++. `std::variant` and the borrow rules are not changed.
- **D2. The annotations** are `WEAVEC_TAGGED_BY(tag[, mask])` and
  `WEAVEC_WHEN(value, …)`, with the program's own tag and no change of
  layout.
- **D3. A tagged union's rules:** a comparison before each member access,
  a store of the tag clears the union, no address of a member, no union
  apart from its tag.
- **D4. The checked conversions** are §2.1's four, and a struct converts
  to its first member's type with no check; the others stay errors.
- **D5. Memory with no record is believed**, and counted; a heap block
  with no type takes the type of its first checked conversion.
- **D6. Every build records an allocation's type** (RFC 0045 §2 amended):
  ABI 49, ledger 14, audit 10.
- **D7. A local whose address leaves a safe function has a frame
  record**, and stays in its plain frame.
- **D8. Measured first.** S0's limits: 1.02 for the interpreter's checked
  casts, 1.05 for checked member reads, 25 ns or an inline path for a
  `void *` conversion, 1.01 for frame records. A limit missed returns to
  the owner before S1.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what S0 and the gates found. They override the body.

1. **The marks (§2.3, _Unresolved question_ 1).** The checker marks each
   checked form in a safe function's AST before its code is made, as RFC
   0048's placed locals are marked: a conversion's operand becomes
   `(T)__weavec.cast((void *)operand, index)`, the struct of a tagged
   member's access `__weavec.member(...)` and of a tag's store
   `__weavec.tag(...)` (an lvalue through its address), each an implicit
   `extern` function with an `asm` label. The marks are taken out while the
   rules read the unit, which see the code as it was written, and put back
   after them for the functions whose code is made at the unit's end. In
   C++ the walk the checker makes anyway finds those functions and the
   sites of §1.1, so no walk is added (G8). GuardInsert expands the marks
   first.
2. **What a checked conversion gives (§2.2).** `__weavec.object(pointer,
   size)`, one object of the target's size: a root GuardSafe bounds by it
   and the fast mode believes, removed in GuardExpand. A base's offset is
   applied to it, so Clang's own null test of a downcast sees null.
3. **A downcast to a class with virtual functions (§2.2).** Read through
   the *source's* vtable (the pointer converted is an object of its class):
   the complete object's type information must be the target's, and its
   offset to top must put the object where the conversion does; else the
   C++ ABI's dynamic cast decides. The draft compared the type information
   at the converted pointer, which a secondary vtable fools (G6's `p15`)
   and which reads a word that may not be a vtable. The checker asks Sema
   for the classes' vtables (`MarkVTableUsed`), so the unit has their type
   information; a unit that still does not is answered by the records.
   A downcast from a class without virtual functions to one with them
   (the source has no vtable to read) is answered by the records, as a
   conversion between classes without them is.
4. **A member's check is made right before each access (§3.3)**, of its
   loads, stores and memory intrinsics, not where its address is computed:
   C may evaluate a call that stores the tag in between (G6's `t25`).
5. **The tag is changed only by an assignment (§3.2).** Every use of a tag
   and of a member is read by where it goes: a read, an assignment,
   `++`/`--`, a member or an element in place, a copy of the whole, or (a
   member without pointers) a pointer to `const`. Its address, a reference
   to it, a member pointer to it, a conditional of it, an argument bound to
   a reference (`std::exchange`), the object of a member call are
   `union-pointer` (G6's `t03`, `t04`, `p01` to `p03`, `p08` to `p11`).
6. **Initializers (§3.2).** An initializer list, a compound literal or a
   global's initializer that sets a member sets the tag to a constant the
   member is for (G6's `t01`, `t07`, `t08`, `p04`). A union given whole
   (`{.kind = TEXT, .u = v}`) is `union-pointer`: its bytes may be another
   member's, which no tag was checked for.
7. **One tag, one union, at most 64 bits** (`bad-annotation`; G6's `t02`,
   `t29`, `p05`). `WEAVEC_WHEN`'s values are taken with their sign, so a
   `long` tag of -1 compares.
8. **A member stored before its tag** is reported only when no store of the
   tag comes before it in the block, and a store of null or zero (a clear)
   is not a member's value (the round's false errors `f04`, `f07`, `f08`).
9. **§3.4, owners.** A store of the tag reports an owner of the union only
   when the function allocated its block and still holds it. The ownership
   flow does not follow the tag, and RFC 0043 follows no owner inside a
   parameter's union: a tag stored over a caller's owner leaks it without
   an error. A leak is not a memory error.
10. **A member with no `WEAVEC_WHEN`** used by safe code is `union-pointer`,
    not a stop. In C++ a tagged union may hold raw pointers (references,
    RFC 0044 §8); a member that is a view class or has a destructor keeps
    the union an error.
11. **Layouts (§1.1).** By the name `getAllocTokenMetadata` gives, built
    from the AST when the unit's allocation sites, globals, recorded locals
    and conversions need them. A type's identity hashes its name, its shape
    (offsets, pointers by what they point to) and, for a type declared in a
    function, its place: two units' `struct node` are one type only when
    they are laid out alike. The parts include every pointer and `bool` of
    the type's own bytes (`WeavecRtPartPointer`), so that a scalar view is
    checked against the bytes it covers, not the whole block (G5's
    `&s->port` taken as an `int *`, a `uint32_t` buffer as bytes).
12. **Every block records its type (§1.2).** `!alloc_token` is asked for in
    every enforcing unit (and in a precompiled header's compile, whose
    options a unit must match); a unit that does not pool records the type
    with `__weavec_rt_note_type` after the allocation; a placed local of
    the checked build too. A block's type is a slot map beside the pools'
    (`slotTypes`), and a huge block's record. A block an array `new`
    puts a cookie in (the C++ ABI's: the element type has a destructor, or
    its usual `operator delete[]` takes the size) records no type, since
    its elements do not begin it; their conversions are believed.
13. **A block is an array of its type** only when its size is a multiple of
    the type's: the bytes after a first object of another size (nng's
    protocol data at `s + 1`) have no record and are believed (G5).
14. **D5, as built.** A block with no type takes the type of its first
    checked conversion at its start, a scalar's too (G6's `t13`: a block
    seen as `long *` and then as a struct with a pointer).
15. **Frame records (§1.2, _Unresolved question_ 3).** In C, a local or a
    parameter whose address is taken in any form but an element read in
    place, scalars included (G6's `t09`, `t10`, `t19`); in C++, a local's
    address made a `void *`, a local seen as a base, and a local (or a
    member of one) of a class this unit derives from bound to a reference.
    Recording every address in C++ cost 15% on tinyxml2 (its `Query*`
    locals). A record covers the whole local (an array's every element,
    G6's `t43`); a thread keeps 1,024. A record goes with its scope, and
    where control lands from frames that are gone: an exception's landing
    pad and the handler that catches it, and every return of a `setjmp`
    (`__weavec_rt_landed` drops the records below its frame), so a frame
    that grows the stack again past them is not answered with their types.
16. **Not built: guarded frames' objects (§1.2's table).** A local of a
    frame of code that is not safe has no record and is believed.
17. **Two more conversions are errors.** A checked form in a global's
    initializer, which runs no check (G6's `t16`), and a reallocation that
    gives a type with pointers from a pointer of another type: its block
    keeps the old bytes (G6's `t14`).
18. **A `void *` handle (§2.1).** A struct that holds pointers may be passed
    as a `void *` parameter without a bound, of a direct or an indirect
    call: the callee reaches its bytes only through a checked conversion.
    `pointer-bytes` had rejected the context pattern in the caller (G4,
    G5's vtables of callbacks). An indirect call's callee is then a safe
    function or code that is not safe, which is trusted to keep the bytes
    it is given: a safe function does not take the address of a library
    function that writes through a pointer argument (`memcpy`, `read`), an
    error of `pointer-bytes`.
19. **E9, as built (§4).** A read under no test does not block the fix (it
    is checked when it runs); a read under another field's test leaves the
    choice to the program; a member that holds a pointer and is read under
    no test makes no fix. Tests are `==`, `switch` labels (a chain of them)
    and `?:`; bit tests are not read.
20. **The ledger and the audit.** The ledger counts the checked conversions
    and members (`checkedCasts`, `checkedMembers`, in the summary line);
    the audit lists the functions that are not safe and only convert and
    call. Which conversions were believed is `WEAVEC_RT_STATS`'s, not the
    audit's.
21. **Reports name a block's type** wherever they describe a heap block,
    in text and in JSON (`"type"`), not only after `bad-cast`.
22. **What a checked conversion gives is bounded by what the records show
    (§2.1, amends amendment 2).** One object of the target's size is what
    the fast mode believes and what an access at a constant offset needs no
    check inside, but the root's bound is the run of objects of the target's
    type that follow one another from the pointer, as the record of what
    holds it shows: `__weavec_rt_cast` returns it, and the marker is
    `__weavec.object(pointer, size, extent)`. The run reaches the end of a
    heap block, global or recorded local that is an array of the type (its
    size a multiple of the type's) from an element's start, and the last
    element of an array member of that type; a plain scalar over a plain
    object runs whole elements to the end; a block with no type that takes
    the conversion's (amendment 14) is an array of it when its size is a
    multiple of the type's. Everything else is one object: a member that is
    not an array, a header with data after it, memory with no record (a
    believed conversion), a null pointer, and a polymorphic downcast, which
    reads a vtable and not a record. The bound is not narrowed by the
    operand's: what a conversion gives is a root of its own, as it was, and
    every byte of the run is of the target's type, so a pointer loaded from
    it is one that typed memory keeps. A local's declaration states no bound
    (RFC 0042 gives it its value's), and the checker lets a converted
    allocation be indexed (`index-single`), so one object stopped a program
    the rules accept: `void *g = realloc(p, 7 * sizeof *p); struct b *q =
    g;` stopped with `pointer-out-of-bounds` at `q[6]`, in every build
    (RFC 0055's adversarial round, G10). A pointer the checker sees as one
    object (a `void *` parameter's, a comparator's) still cannot be
    indexed. `WEAVEC_RT_ABI` is raised (the entry point returns a value);
    pinned by `test/Guards/cast-extent.c`, `ir-cast.c`, cases
    `test/cases/sum/cast-grown*.c` and `rt_test` `type-at-run`.

## Measured gates

darwin-arm64, Homebrew LLVM 23.1.0, main at `8a305f8d` (RFC 0053, PR 80),
2026-11-22. Cost gates use Release compilers of main (`build/rfc53/rel-branch`)
and this branch (`build/rfc54/rel`); the agents of G3 to G6 had a frozen
copy of this branch's compiler as amendments 1, 2 and 12 left it
(`build/rfc54/tc`). Evidence is in `build/rfc54/` (not committed). The
library grows by 2,948 lines of code (30,338 to 33,286), in
`SafeTypes.cpp`, `GuardTypes.cpp`, `SafeCheck.cpp` and `SafeFix.cpp`; the
runtime by 419 (8,786 to 9,205), in `weavec_types.c` and `weavec_alloc.c`
(G9, with the review's fixes; the draft expected 1,600 and 500).

**S0: passes.** (a) the marks reach the passes (amendment 1); (b) the
trial interpreter's 41 `static_cast`s checked cost 1.009 of the same build
unchecked (`-mllvm -weavec-type-checks=0`; limit 1.02); (c) a bytecode loop
over a tagged value, its member reads checked, 1.006 (limit 1.05); (d) a
`void *` conversion of a heap block, 4.3 ns (limit 25 ns: no inline path);
(e) frame records, within G7's numbers below.

**G1, tests: passes.** As §7 lists, with `test/Safe/tagged-union-uses.c`
and `cxx-tagged-union-uses.cpp` for the round's holes, and `rt_test`'s
`union-member`.

**G2, the survey: fails.** Of the 1,014 `pointer-cast` errors, 583 are gone
with no edit (asks 600): Lua keeps 334, its `GCObject` and objects that
begin with the same macro's fields (_Unresolved question_ 4). After
`--weavec-fix`, 402 of the 1,853 `union-pointer` errors are gone (asks
half): libyaml 168 to 24, hiredis 16 to 0, wren 9 to 0, Lua 1,222 to 989
(its tag carries variant bits, tested by masks in macros, which E9 does not
read), mujs 438 to 438 (its union's tag overlaps the short string's bytes,
by design: no field of a struct is its tag).

**G3, the interpreter as written: passes.** It has 38 rule errors, none
`pointer-cast`. A fresh agent made it safe in about 6 minutes (limit 6),
693 of 694 functions safe, its casts kept; the fast build runs the
benchmark in 1.62 times the program as written (limit 1.70; RFC 0051's
rewrite with `dynamic_cast`s, 1.93 then, 2.05 now).

**G4, new C: fails.** A fresh agent wrote Python's `json` in Safe C in
about 7.5 minutes and 3 rounds, 73 of 77 functions safe (the 4 others only
read and write files), a tagged union for the value and no guarded
function for a union or a conversion; `check.py` passes in both builds but
for one case no program can pass (a lone surrogate the checker decodes
with `replace`). Its benchmark runs in 1,303 ms in the fast build, 3.0
times Rust's port (asks 1.5); Clang's build of the same C is 1.55 times
Rust's, so most of the gap is the port's own code.

**G5, a conversion: passes.** On nng as RFC 0049 left it, an agent made
2,514 of 2,917 functions safe (86%; 60% at its start with this compiler)
in 30 minutes, tests passing in both builds: 632 adapters made safe, 86
list accessors, 29 typed getters, a tagged union. Its run-time stops were
the false stops amendments 11, 13 and 18 then removed, and `sockaddr_storage`
taken as a `sockaddr_in`, two plain structs of different types, which
stays a stop (10 functions guarded). It found that
`definition-mismatch` (RFC 0051) tells `bool` from `_Bool`, which is not
this RFC's.

**G6, a round: fails.** Two agents wrote 70 probes; the C++ agent was
stopped by a classifier after 18 and not run again. Of 33 holes, 29 are
closed by amendments 3 to 7, 14, 15 and 17 and are pinned by tests; 4
remain, each memory with no record of its type, which S-A2 believes: a
compound literal's address, a `thread_local` global, an address in no
record on the stack (`container_of` before a recorded local), and two
local types of one name in one unit that the allocation's name does not
tell apart. The checked build stops the last two at the access. The
round's false errors were fixed (amendments 8, 18), but for `qsort` of an
array of structs that hold pointers, `pointer-bytes` of RFC 0042's library
rows. A review of the branch after the round found six more holes, all
closed and pinned: a downcast from a base with no virtual functions read
its first field as a vtable (amendment 3); a safe C function could pass
`memcpy`'s address to a callback that forged a handle's pointers
(amendment 18); a union initialized whole kept a member its tag was not
for (amendment 6); the bases of a template's instances, a local class's
and those in `extern "C++"` were not known, so a downcast to one was not
checked; an array `new` whose cookie comes before its elements recorded
the type at the cookie (amendment 12); and a frame left by `longjmp` or a throw kept its records,
so a correct conversion in a later frame stopped (amendment 15). Two were
left: same-named static locals in one C unit share a layout's name, and a
conversion to a global's type scans the unit's globals linearly.

**G7, cost elsewhere: fails narrowly.** Medians of 9, interleaved, with the
final compilers, against main's fast and checked builds: the json port
0.999 and 1.019, RFC 0049's json port 1.001 and 1.016, the LRU port 1.019
and 1.003, tinyxml2 1.005 and 1.005, the trial interpreter 1.001 and 1.012
(limits 1.01 and 1.02); a guarded Lua 1.012 (limit 1.02). The LRU port's
fast build is 1 ms over on 70 ms, and was 1.014 and 1.010 in the two
earlier runs. Amendment 15's first form, every address of a C++ local
recorded, cost tinyxml2 1.159. These were measured before the review's
fixes, which add work only where a frame is left by a landing pad, a
catch or a return from `setjmp`, and drop the record of an array `new`
with a cookie; G7 was not measured again.

**G8, compile time: passes.** CPU time of compiling the trial interpreter,
two json ports, tinyxml2, the safe and ownership cases and Lua: 1.006 of
main's (limit 1.02).

Status stays **Accepted**: G2, G4 and G6 fail, and G7 by 1 ms.
