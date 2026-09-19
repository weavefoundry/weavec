---
title: Safety guarantees and scope
description: What a translation unit compiled by weavec-cc is guaranteed, the five assumptions behind it, the blame property, and what stays outside it.
---

WeaveC records exactly one outcome for every safety facet of every memory operation it compiles. The guarantee below is stated in terms of those outcomes. It is specified by [RFC 0030, _Soundness_](/rfcs/0030-prove-or-trap/#soundness), which is authoritative; this page restates it.

## Outcomes

Each operation site has up to four _facets_: **spatial** (the bytes it touches lie inside its object), **null** (it does not dereference null), **temporal** (the object is still alive, and a release releases a live allocation once) and **assertion** (a `WEAVEC_ASSUME`). Each facet gets one outcome:

| Outcome      | Meaning                                                                                                                       |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------- |
| `proven`     | The facet holds on every execution, under the assumptions below.                                                              |
| `checked`    | Not proven. A compiler-inserted check traps before the operation if the facet fails.                                          |
| `violation`  | The facet fails on every execution that reaches the site. It is always a build error.                                         |
| `unresolved` | Neither proven nor checkable. The ledger gives a reason from a closed list, such as `unknown-extent` or `unknown-callee`.     |
| `trusted`    | Holds if a named trust assumption holds, such as `unsafe` (a `WEAVEC_UNSAFE` region) or `system-api` (a platform C function). |

The [ledger](/reference/cli/#ledger-and-summary-line) lists every facet with its outcome, and the summary line counts them. With `-fweavec-checks=none`, and in the `weavec` analysis tool, no check is emitted: `checked` facets are reported as "checkable (not enforced)".

## The guarantee

Let _U_ be a translation unit compiled by `weavec-cc` in an **enforcing mode**: `-fweavec-checks=trap` (the default) or `verify`, or `report` with `WEAVEC_RT_ABORT=1` in the environment, with zero-initialisation on (the default). For every execution of a program containing _U_'s object code, and every operation site _s_ emitted from _U_ outside a `WEAVEC_UNSAFE` region:

- **(S) Spatial.** If the spatial facet of _s_ is proven or checked, the bytes _s_ accesses lie inside the object its pointer was derived from. For a checked facet, the program instead traps at _s_ before the access.
- **(N) Null.** If the null facet of _s_ is proven or checked, _s_ does not dereference a null pointer. For a checked facet, the program instead traps first.
- **(T) Temporal.** If the temporal facet of _s_ is proven, the object _s_ accesses has not been released and its lifetime has not ended. For a release, the object is released at most once and is the start of a live allocation of the releasing family.
- **(V) Violations.** No violation reaches emitted code unguarded. A violation fails the compile; if its error is lowered to a warning with `-Wno-error=weavec-<id>`, the site is emitted behind a check that traps.

`-fweavec-checks=report` without `WEAVEC_RT_ABORT=1` is a rollout aid, not an enforcing mode: a failed check is reported and the access proceeds, so (S) and (N) do not hold for its checked facets. Its ledger is the same as in trap mode, because the ledger always describes the enforcing build.

Temporal facets are never checked at run time. A possible temporal bug is a warning and an `unresolved` row, not a trap.

## Assumptions

The guarantee holds under five assumptions:

- **A1 — callers.** Callers outside _U_ of its exported and address-taken functions pass arguments that satisfy what the callee relies on: each pointer is null or satisfies the callee's kind (its declared kind, or by default at least one live object of its pointee type); declared requirements hold; no two arguments _U_ treats as owning refer to the same object.
- **A2 — trusted callees.** Every callee whose effect a facet records as trusted behaves as its contract says: `system-api`, `library-spec`, `extern-contract` or `external-unit`. Values stored from outside into a function-pointer slot with known targets behave like those targets.
- **A3 — other code maintains the heap invariants.** Code outside _U_ leaves every pointer _U_ can reach through parameters, results and globals either null or pointing to a live object with at least one element of its type. Pointers in owning slots are unique. Exact counted-field invariants on header structs that _U_ relies on hold.
- **A4 — no concurrency outside trust.** No other thread, signal handler or `longjmp` changes memory _U_ accesses during _U_'s operations, except at sites marked for it: `trusted(concurrency)` or `unresolved(setjmp)` facets, and the checked facets substituted for flow proofs there.
- **A5 — initialisation.** The linked allocator answers the usable-size query (`malloc_usable_size`, `malloc_size`) consistently with its `malloc`. Pointer-typed memory _U_ reads that did not come from a zero-initialising source (automatic storage whose declaration no jump bypasses, static storage, and the allocation calls `weavec-cc` lowers) was written before _U_ loads it. This covers memory from other allocators, unknown callees and the program's own free lists.

When `weavec-cc` links objects that carry WeaveC records, the link step verifies A1 and A3 where the callers and stores are visible, and its summary line lists what remains unverified. Link inputs without a record are named in one `unanalyzed-input` warning; calls into them are `trusted(external-unit)`.

## The blame property

Suppose an execution of _U_'s code performs a memory-safety violation at site _s_: an out-of-object access, a null dereference, or a use or release of a released object. Then at least one of these holds:

1. the program trapped at a checked site first;
2. the violated facet of _s_ is unresolved or trusted;
3. the fact that proved _s_ was established at a creation point, store, call, boundary or function entry whose own facet is unresolved or trusted, or that rests on A1–A5 at an interface of _U_;
4. WeaveC has a bug.

The ledger names the site in cases 2 and 3, though in case 3 not always the nearest cause: it records outcomes, not proof dependencies. `-fweavec-checks=verify` is the monitor for case 4: it also checks proven facets where it can, and a `weavec.proven` trap means the analysis proved something false.

## What is caught

At compile time, as errors, when they hold on every path:

- use of a released or moved object, a double release, releasing non-heap or interior storage, and releasing through the wrong allocator family;
- dereferencing a pointer that is null on every path from a non-allocator source;
- an access past an object of exact extent that is out of bounds for every value the facts allow, including library calls with a known required length, overlapping `memcpy`/`strcpy`-family copies, writes into string literals and format strings that read more arguments than are passed;
- a contradicted `WEAVEC_ASSUME`, and a declaration whose annotation contradicts its definition, including across translation units at link.

At run time, by a trap at the offending access: every dereference whose null outcome is not proven, and every index, cursor dereference and library-call length whose extent is exact or declared and can be named at the site.

As warnings, with "may" wording: temporal bugs that happen on some paths only.

## What is not caught

- **Temporal bugs at run time.** There is no quarantine allocator or memory tagging; an unproven temporal facet is a warning or an unresolved row.
- **Accesses without an exact or declared extent**, such as a header before the pointer (`s[-1]`), `container_of`, or a buffer from an unknown callee indexed beyond its first element. They are `unresolved(unknown-extent)` or `unresolved(unknown-index)`. Declare the extent with `WEAVEC_COUNTED_BY` to make them checkable.
- **Pointers made by reinterpretation** (union punning, byte-wise copies of pointers, `va_arg` of pointer type): `unresolved(raw-cast)`. A pointer converted from an integer is raw and needs a `WEAVEC_UNSAFE` region.
- **Uninitialised scalars.** The enforcing modes define them as zero; with `-fno-weavec-zero-init` they are not covered.
- **Data races and asynchronous signals** (assumption A4), and **`longjmp` into a dead frame**.
- **Leaks.** A leak is a warning and never part of the guarantee.
- **Integer overflow as such, type confusion within an object, floating point and inline assembly.** A size overflow is caught only where it leads to an out-of-bounds access.
- **Code without WeaveC records** (archives, shared libraries, objects from another compiler): trusted, and named at link.

## Tightening the result

Trusted facets are allowed at every level; trust is explicit and listed in the ledger. `-fweavec-require` makes the rest fail closed:

| Level     | An error for each facet that is | Diagnostic                                    |
| --------- | ------------------------------- | --------------------------------------------- |
| `none`    | nothing (the default)           |                                               |
| `checked` | unresolved                      | `unresolved-operation`                        |
| `proven`  | unresolved or checked           | `unresolved-operation`, `unchecked-operation` |

`WEAVEC_REQUIRE_SAFE` before a function definition holds that function to `checked` whatever the command line says. `-fweavec-require=proven` is the only fail-closed level without runtime checks.

Possible temporal findings are warnings, so a build can succeed with a real path-dependent use-after-free; the warning and the ledger keep it visible, and `-fweavec-require=checked` restores fail-closed behaviour. With the `weavec` tool or `-fweavec-checks=none`, possible null and spatial findings produce no diagnostic and no check: they are counted as "checkable (not enforced)". The checks in a `weavec-cc` build, not the `weavec` tool alone, are what enforce these classes.
