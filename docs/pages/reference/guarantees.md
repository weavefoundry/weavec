---
title: Safety guarantees and scope
description: What a translation unit compiled by weavec-cc is guaranteed, what its runtime guards and at what cost, the six assumptions behind it, the blame property, and what stays outside it.
---

WeaveC records exactly one outcome for every safety facet of every memory operation it compiles. The guarantee below is stated in terms of those outcomes. It is specified by [RFC 0030, _Soundness_](/rfcs/0030-prove-or-trap/#soundness) and, for the runtime and the guarded outcome, [RFC 0032, _Soundness_](/rfcs/0032-runtime-enforcement/#soundness), which are authoritative; this page restates them.

## Outcomes

Each operation site has up to four _facets_: **spatial** (the bytes it touches lie inside its object), **null** (it does not dereference null), **temporal** (the object is still alive, and a release releases a live allocation once) and **assertion** (a `WEAVEC_ASSUME`). Each facet gets one outcome:

| Outcome      | Meaning                                                                                                                                                                                          |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `proven`     | The facet holds on every execution, under the assumptions below.                                                                                                                                 |
| `checked`    | Not proven. A compiler-inserted check traps before the operation if the facet fails.                                                                                                             |
| `guarded`    | Not proven and not checkable from what the code states. A guard looks the pointer up in the runtime's object table and traps under clause (G) below. The row keeps the reason the analysis gave. |
| `violation`  | The facet fails on every execution that reaches the site. It is always a build error.                                                                                                            |
| `unresolved` | Not proven, checkable or guardable. The ledger gives a reason from a closed list, such as `raw-cast` or `unknown-callee`.                                                                        |
| `trusted`    | Holds if a named trust assumption holds, such as `unsafe` (a `WEAVEC_UNSAFE` region) or `system-api` (a platform C function).                                                                    |

The [ledger](/reference/cli/#ledger-and-summary-line) lists every facet with its outcome, and the summary line counts each site by its worst facet, in the order violation, unresolved, guarded, checked, trusted, proven. With `-fweavec-checks=none`, and in the `weavec` analysis tool, no check or guard is emitted: `checked` facets are reported as "checkable (not enforced)" and `guarded` facets as "guardable (not enforced)". With `-fno-weavec-runtime` nothing is guarded and those facets are `unresolved`.

`checked` and `guarded` are kept apart because they promise different things. A check compares the operation with a bound or a fact the code states, and (S) and (N) hold for it without exception. A guard compares it with what the runtime knows about the object the pointer points into at that moment, and holds only as far as (G) says.

## The guarantee

Let _U_ be a translation unit compiled by `weavec-cc` in an **enforcing mode**: `-fweavec-checks=trap` (the default) or `verify`, or `report` with `WEAVEC_RT_ABORT=1` in the environment, with zero-initialisation on (the default). For every execution of a program containing _U_'s object code, and every operation site _s_ emitted from _U_ outside a `WEAVEC_UNSAFE` region:

- **(S) Spatial.** If the spatial facet of _s_ is proven or checked, the bytes _s_ accesses lie inside the object its pointer was derived from. For a checked facet, the program instead traps at _s_ before the access.
- **(N) Null.** If the null facet of _s_ is proven or checked, _s_ does not dereference a null pointer. For a checked facet, the program instead traps first.
- **(T) Temporal.** If the temporal facet of _s_ is proven, the object _s_ accesses has not been released and its lifetime has not ended. For a release, the object is released at most once and is the start of a live allocation of the releasing family.
- **(G) Guards.** For a unit compiled with the runtime (the default). If the spatial facet of _s_ is guarded, and the pointer operand of _s_ points into a tracked object _O_ (or, for the base of a subscript, one past its end), then _O_ is live and the bytes _s_ accesses lie inside _O_, or the program traps at _s_ before the access. If the temporal facet of _s_ is guarded and the pointer operand points into a dead tracked object, the program traps at _s_ before the access. A guarded release is of null, of the start of a live heap object (which the release makes dead), or of a pointer to no tracked object; otherwise the program traps.
- **(V) Violations.** No violation reaches emitted code unguarded. A violation fails the compile; if its error is lowered to a warning with `-Wno-error=weavec-<id>`, the site is emitted behind a check that traps.

`-fweavec-checks=report` without `WEAVEC_RT_ABORT=1` is a rollout aid, not an enforcing mode: a failed check or guard is reported and the access proceeds, so (S), (N) and (G) do not hold for its checked and guarded facets. Its ledger is the same as in trap mode, because the ledger always describes the enforcing build.

(G) is weaker than (S) and (T) in three ways:

1. **Provenance is the pointer's value.** A guard asks which object the pointer points into now, not which object it was derived from. Pointer arithmetic that leaves object _A_ and lands inside live tracked object _B_ passes a guard at a plain dereference. A subscript `p[i]` is checked against the object that contains `p`, so an index that leaves that object traps wherever it lands. Between heap objects there is always at least one byte that belongs to no object, so a walk that leaves a heap object one element at a time, forwards, traps; adjacent stack or global objects have no such gap.
2. **Recycling.** A released heap object stays dead while it is in the quarantine. Once its storage has been given to a new object, a stale pointer finds that object and the guard passes.
3. **Untracked memory.** A guard passes on a pointer outside every tracked object (assumption A6).

## The runtime

Every enforcing link carries the runtime: `libweavec_rt.a`, the object table, and `libweavec_alloc.a`, which defines `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions for the image. Pointer representation and the ABI do not change. A _tracked object_ is one of:

- a **heap object**: a block the image's allocator returned and has not recycled, from its start to its requested size. It is _live_ from its allocation to its release, and _dead_ from its release until the quarantine gives its storage to a new object;
- a **stack object**: a local variable or a parameter of a function compiled with the runtime whose address is taken or which decays to a pointer, from its declaration to the end of its scope. After its scope ends it is untracked, never dead: the compile-time `lifetime-too-short` rules cover the stack's temporal bugs;
- a **global object**: a variable of static storage duration defined in a unit compiled with the runtime, other than a thread-local, a string literal or an object with a flexible array member.

Everything else is untracked, and a guard passes on it: string literals, `alloca` blocks, memory from another allocator or another image's allocator, memory created by code built without the runtime, and stack objects in functions that call `setjmp`, in coroutines, on alternate signal stacks and in functions compiled from a precompiled header.

The allocator serves each block from a size class strictly larger than the request, so its extent is exact and one past its end is still inside its slot. It zero-fills every block. Released blocks wait in a first-in, first-out quarantine with a byte budget (64 MiB by default, `WEAVEC_RT_QUARANTINE=<bytes>` in the environment); `malloc_usable_size` and `malloc_size` return the requested size. The allocator itself validates every release in the image, in whichever object the `free` is: a double free, a free of an interior, stack or global pointer, and a `realloc` of a dead block stop the program.

On ELF targets the executable's allocator serves every shared library in the process. On Darwin it serves its own image, and the system libraries keep theirs; blocks that cross between them are handed to the allocator that owns them.

### What it costs

Guards run each time the operation they cover runs, and each escaping local is registered and removed on each activation of its function. Measured on the project's benchmarks, as user CPU time and peak memory relative to the same program built by the reference Clang ([RFC 0032](/rfcs/0032-runtime-enforcement/#implementation-amendments), amendment 3):

| Benchmark         | Default (runtime) | `-fno-weavec-runtime` | Peak memory (default) |
| ----------------- | ----------------- | --------------------- | --------------------- |
| cJSON parse/print | 1.66×             | 1.14×                 | 0.59×                 |
| zlib minigzip     | 1.85×             | 1.00×                 | 1.09×                 |
| Lua bench         | 5.94×             | 1.11×                 | 1.61×                 |

The cost follows the number of guards executed, not the cost of one. Interpreters and tight loops over pointers pay the most: the Lua benchmark executes roughly one guard for every two instructions of the unguarded program. The RFC's own bounds for the default mode (1.5× for zlib, 2.0× for Lua) are not met and remain an open gate; closing it needs guards removed at compile time, which is future work.

For code that cannot pay, `-fno-weavec-runtime` is the way out. It can be applied per unit. In such a unit no guard is emitted and nothing is registered; the facets that would be guarded are `unresolved`; (G) does not apply, so no access of the unit is checked for a freed object at run time; and (S), (N), (T) and (V) hold as before. Releases are still validated as long as the link carries the runtime's allocator. When the link also has `-fno-weavec-runtime`, the system allocator is used and A5 again rests on its usable-size query. Each proof or check that replaces a guard (a declared extent, an ownership annotation, a linked definition) removes that guard's cost without giving anything up.

### When the runtime is not used

`weavec-cc` builds without the runtime, and says so in a note at the link, under a sanitizer that replaces the allocator, with `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, and for targets other than 64-bit Darwin and Linux. A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its allocator: guards are still emitted and stack and global objects are still tracked, but the heap is untracked, so guards pass on heap pointers and releases are not validated. The link prints a note in that case too. The notes are quoted in the [command-line reference](/reference/cli/#the-runtime).

## Assumptions

The guarantee holds under six assumptions:

- **A1 — callers.** Callers outside _U_ of its exported and address-taken functions pass arguments that satisfy what the callee relies on: each pointer is null or satisfies the callee's kind (its declared kind, or by default at least one live object of its pointee type); declared requirements hold; no two arguments _U_ treats as owning refer to the same object.
- **A2 — trusted callees.** Every callee whose effect a facet records as trusted behaves as its contract says: `system-api`, `library-spec`, `extern-contract` or `external-unit`. Values stored from outside into a function-pointer slot with known targets behave like those targets.
- **A3 — other code maintains the heap invariants.** Code outside _U_ leaves every pointer _U_ can reach through parameters, results and globals either null or pointing to a live object with at least one element of its type. Pointers in owning slots are unique and acyclic: no object is reachable from itself by following owning slots only (the owner forest of [RFC 0031](/rfcs/0031-object-engine/#soundness)). Exact counted-field invariants on header structs that _U_ relies on hold.
- **A4 — no concurrency outside trust.** No other thread, signal handler or `longjmp` changes memory _U_ accesses during _U_'s operations, except at sites marked for it: `trusted(concurrency)` or `unresolved(setjmp)` facets, and the checked facets substituted for flow proofs there.
- **A5 — initialisation.** In a program that does not use the runtime's allocator, the linked allocator answers the usable-size query (`malloc_usable_size`, `malloc_size`) consistently with its `malloc`; the runtime's allocator zero-fills every block it returns, and needs no such assumption. Pointer-typed memory _U_ reads that did not come from a zero-initialising source (automatic storage whose declaration no jump bypasses, static storage, and the allocation calls `weavec-cc` lowers) was written before _U_ loads it. This covers memory from other allocators, unknown callees and the program's own free lists.
- **A6 — untracked memory.** A pointer that a guard finds outside every tracked object points to an object the image's WeaveC units did not create (a string literal, an `alloca` block, memory another image or another allocator owns, memory created by a unit built without the runtime), which is live and which the access stays inside.

When `weavec-cc` links objects that carry WeaveC records, the link step verifies A1 and A3 where the callers and stores are visible, and its summary line lists what remains unverified. Link inputs without a record are named in one `unanalyzed-input` warning; calls into them are `trusted(external-unit)`.

## The blame property

Suppose an execution of _U_'s code performs a memory-safety violation at site _s_: an out-of-object access, a null dereference, or a use or release of a released object. Then at least one of these holds:

1. the program trapped at a checked site first;
2. the violated facet of _s_ is unresolved or trusted, or guarded and the violation falls under one of the three limits of (G);
3. the fact that proved _s_ was established at a creation point, store, call, boundary or function entry whose own facet is unresolved or trusted, or that rests on A1–A6 at an interface of _U_;
4. WeaveC has a bug.

The ledger names the site in cases 2 and 3, though in case 3 not always the nearest cause: it records outcomes, not proof dependencies. `-fweavec-checks=verify` is the monitor for case 4: it also checks proven facets where it can, and with the runtime it guards proven temporal facets and the proven spatial facets that have no static check. A `weavec.proven` trap means the analysis proved something false, or that a caller broke an assumption the proof rests on (A1).

## What is caught

At compile time, as errors, when they hold on every path:

- use of a released or moved object, a double release, releasing non-heap or interior storage, and releasing through the wrong allocator family;
- dereferencing a pointer that is null on every path from a non-allocator source;
- an access past an object of exact extent that is out of bounds for every value the facts allow, including library calls with a known required length, overlapping `memcpy`/`strcpy`-family copies, writes into string literals and format strings that read more arguments than are passed;
- a contradicted `WEAVEC_ASSUME`, and a declaration whose annotation contradicts its definition, including across translation units at link.

At run time, by a check that traps at the offending access: every dereference whose null outcome is not proven, and every index, cursor dereference and library-call length whose extent is exact or declared and can be named at the site.

At run time, by a guard that traps at the offending operation, within the limits of (G):

- an index or a cursor that leaves a heap block, an escaping local or a global whose extent the code does not state, such as `v->data[i]` or a `char *dst` walked past the caller's `char buf[8]`;
- a library call such as `memcpy(dst, src, n)` whose `n` exceeds either object, where the extents are unknown statically;
- a use of a heap object after it was released, including through an alias the analysis could not follow and after a `realloc` moved the block;
- a double free, a free of an interior, stack or global pointer, and a `realloc` of a released block. The allocator validates these for every release in the image, guarded or not.

As warnings, with "may" wording: temporal bugs that happen on some paths only. `weavec` prints all of them. A `weavec-cc` build with the runtime does not print the ones whose facet is guarded, because the guard traps if the bug happens; `-Wweavec-possible` prints them. Leak warnings, and possible findings on facets that stay unresolved, are always printed.

## What is not caught

- **The three limits of a guard.** A pointer that arithmetic moved from one live tracked object into another, at a plain dereference; a use of a freed heap block after the quarantine recycled its storage; an access through a pointer into memory the runtime does not track.
- **Under-runs and walks at globals.** A negative index from the start of a global, and a walk off the end of a global one element at a time, pass a guard: the neighbouring bytes may belong to an object the runtime does not track. On the stack and on the heap both trap.
- **An array type larger than its block.** An access through a pointer to an array, such as `(*pp)[k]` with `pp` of type `int (*)[4]`, is checked against the type's bound. If the block `pp` points to is smaller than the array type, an index inside the type and outside the block is not caught.
- **Bit-fields at the end of a truncated record.** A guard covers the bytes a bit-field's bits lie in; the generated access may read or write its whole storage unit.
- **Temporal facets at call boundaries.** The temporal facet of a call site is about every object reachable from the arguments and globals, not about one pointer. It has no guard and stays `unresolved`. The uses that depend on it are guarded only where their own facet is not proven: a callee that receives a pointer to a released object through a typed parameter can have its dereference proven under A1, and then nothing traps.
- **Pointer arithmetic, casts and integer-to-pointer conversions.** These sites have no guard; the accesses through the resulting pointers do.
- **Sub-object overflows.** A check or a guard covers the whole object: an overflow from one field of a struct into the next is inside the object.
- **Everything, at run time, in a unit built with `-fno-weavec-runtime`** beyond its checks: accesses without an exact or declared extent (a header before the pointer `s[-1]`, `container_of`, a buffer from an unknown callee) are `unresolved(unknown-extent)` or `unresolved(unknown-index)`, and no temporal facet is enforced. Declaring the extent with `WEAVEC_COUNTED_BY` makes such an access checkable with or without the runtime.
- **Pointers made by reinterpretation** (union punning, byte-wise copies of pointers, `va_arg` of pointer type): `unresolved(raw-cast)`. A pointer converted from an integer is raw and needs a `WEAVEC_UNSAFE` region.
- **Uninitialised scalars.** The enforcing modes define them as zero; with `-fno-weavec-zero-init` they are not covered.
- **Data races and asynchronous signals** (assumption A4), and **`longjmp` into a dead frame**. A release by another thread or a signal handler while a loop that makes no calls is running is not seen by that loop's guards until the loop is next entered.
- **Leaks.** A leak is a warning and never part of the guarantee.
- **Integer overflow as such, type confusion within an object, floating point and inline assembly.** A size overflow is caught only where it leads to an out-of-bounds access.
- **Code without WeaveC records** (archives, shared libraries, objects from another compiler): trusted, and named at link.

## False traps

A guard never compares against a bound the runtime does not know to be exact: a heap object's extent is its requested size, a stack or global object's is its type's size, and anything else is untracked and passes. The width of a guarded access is the bytes the access touches (`p->f` guards `sizeof` the field at its offset), so a struct allocated without its unused tail is not a false trap, and a zero-length library access passes without a lookup.

A correct program can still trap when it breaks C's rules in a way that happens to work:

- reading a word at a time past the end of a string's allocation;
- using a block's tail beyond its requested size (`malloc_usable_size` returns the requested size).

Such code needs a `WEAVEC_UNSAFE` region or a fix. Comparing or subtracting pointers into a released block does not trap; only an access through one does. A program that depends on the system allocator's identity (a `malloc_zone_t` of its own, allocator introspection, `mallopt`) sees WeaveC's allocator instead. One residual false trap is known: when code built without the runtime calls `setjmp` and a `longjmp` skips the scope exits of functions built with it, stale stack entries remain until the stack is next entered at or above their frame, and a guard that meets one can trap. `-fno-weavec-stack-objects` turns stack tracking off for a unit.

## Tightening the result

Trusted facets are allowed at every level; trust is explicit and listed in the ledger. `-fweavec-require` makes the rest fail closed:

| Level     | An error for each facet that is | Diagnostic                                    |
| --------- | ------------------------------- | --------------------------------------------- |
| `none`    | nothing (the default)           |                                               |
| `guarded` | unresolved                      | `unresolved-operation`                        |
| `checked` | unresolved or guarded           | `unresolved-operation`                        |
| `proven`  | unresolved, guarded or checked  | `unresolved-operation`, `unchecked-operation` |

`WEAVEC_REQUIRE_SAFE` before a function definition holds that function to `checked` whatever the command line says. `-fweavec-require=checked` is the level at which every facet is proven or holds without the limits of (G). `-fweavec-require=proven` is the only fail-closed level without runtime checks.

In a default `weavec-cc` build, a possible temporal finding on a guarded facet is not printed, so a build can succeed without a warning for a real path-dependent use-after-free. The bug traps when it happens, within the limits of (G); the ledger row keeps the reason; `-Wweavec-possible` prints the warning; and `-fweavec-require=checked` fails the build on every guarded facet. Possible findings on facets that stay unresolved are warnings as before. With the `weavec` tool or `-fweavec-checks=none`, possible null and spatial findings produce no diagnostic, no check and no guard: they are counted as "checkable (not enforced)" and "guardable (not enforced)". The checks and guards in a `weavec-cc` build, not the `weavec` tool alone, are what enforce these classes.
