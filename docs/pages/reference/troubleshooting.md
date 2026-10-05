---
title: Troubleshooting
description: Resolve installation problems, missing compilation flags, runtime traps, guard failures, runtime fallback notes, slow programs and builds, unresolved ledger rows, and link inputs without WeaveC records.
---

## CMake cannot find LLVM or Clang

Install the development libraries, not just a compiler executable. Set `WEAVEC_LLVM_PREFIX` to the LLVM installation prefix, or supply `CMAKE_PREFIX_PATH`, `LLVM_DIR`, and `Clang_DIR`. On macOS, use Homebrew LLVM; Xcode does not ship the required CMake packages.

Use a fresh build directory if a previous configuration selected a different LLVM installation. See the [developer toolchain guide](/contributing/development/#toolchain).

## FileCheck or lit is missing

Tests require `lit` plus LLVM's `FileCheck`, `not`, and `count` utilities. Make sure the chosen LLVM tool directory is on PATH and use a matching `lit` release. Building LLVM from source requires its utilities to be installed.

## The installed compiler cannot locate Clang resources

Keep the LLVM installation used at configure time available. The driver records its resource directory and Clang executable. `WEAVEC_RESOURCE_DIR` and `WEAVEC_CLANG` can override those locations when using a compatible installation. See [build targets](/contributing/development/#building).

## Headers or macros are missing during analysis

Use a current `compile_commands.json` with `-p build`, or pass your actual include directories and defines after `--`:

```sh
weavec src/main.c -- -std=c17 -Iinclude -DPROJECT_FEATURE=1
```

Resolve Clang parse errors before interpreting analysis results.

## A helper's behavior is not visible

Analyze its source with the caller using `weavec --whole-program`, or link with `weavec-cc -fweavec-link=analyze`; a default link reads the units' records and does not analyse them again. An unannotated declaration alone does not describe an unavailable function: WeaveC assumes it may free, keep or replace its pointer arguments, and the ledger lists the affected operations with the reason `unknown-callee` (guarded where a guard covers them, unresolved otherwise) and a suggested annotation.

For callbacks, ensure the actual targets are stored somewhere the program can see. A function-pointer type alone does not say which function a call reaches.

## A program built with weavec-cc traps

A trap (`SIGTRAP` or `SIGILL`) means a runtime check or guard failed: a null dereference, an out-of-bounds access, a use of a freed object or an invalid `free` was about to happen. Rebuild with `-fweavec-checks=report` and rerun; each failure prints `weavec: runtime check failed: <template> at <file>:<line>:<column>` and the program continues, so one run shows every failing site. The ledger row at that line says what was checked.

Usually the trap is a real bug. Sometimes the code relies on undefined behavior that happens to work, such as reading one element past an array; fix it, or move the operation into a narrow `WEAVEC_UNSAFE` region. Declared extents are enforced too: a call that passes less than a `WEAVEC_COUNTED_BY(n)` parameter promises can trap at the call.

## A guard fails: object, live or release

The templates `object`, `live` and `release` are guards: the runtime looked the pointer up in its table of heap, stack and global objects.

- `object`: the access leaves the object its pointer points into, or that object has been freed. For `p[i]` the object is the one that contains `p`. Look for an index or length that exceeds the allocation, or a pointer kept across a `free` or a `realloc` that moved the block.
- `live`: the pointer points into a heap block that has been freed.
- `release`: the argument of `free` (or another heap releaser) is not null and not the start of a live heap block: a double free, or a free of an interior, stack or global pointer.

A guard compares against the size the program asked the allocator for. Code that is correct on the system allocator can fail one when it reads a word at a time past the end of a string's allocation, or uses the slack `malloc_usable_size` used to report (it now returns the requested size). Fix the code or put the access in a `WEAVEC_UNSAFE` region. If a guard fails on a stack object after a `longjmp` out of code that was not built by `weavec-cc`, the stack list holds a stale entry; build the affected unit with `-fno-weavec-stack-objects`.

## The program stops with "weavec: invalid release"

```text
weavec: invalid release of 0x7d00000000: the block was already released
```

The runtime's allocator received a `free` or `realloc` it cannot honour, from a site without a guard, usually in an object another compiler built. The reasons are `the block was already released`, `no block is allocated there`, `not the start of its block`, `not the start of a live block` and `not a heap block`. Run the program under a debugger to find the caller; the fix is the same as for a failed `release` guard.

## The link prints "building without the WeaveC runtime"

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): guardable facets stay unresolved
```

The command line has something the runtime cannot work with: a sanitizer that replaces the allocator, `-ffreestanding`, `-nostdlib`, `-nodefaultlibs`, `-nolibc`, or a target other than 64-bit Darwin or Linux. The build is what `-fno-weavec-runtime` would produce: the checks remain, nothing is guarded, and the summary line shows `0 guardable (not enforced)` with those facets counted as unresolved. Nothing is wrong with the build; the note records that it enforces less. Pass `-fno-weavec-runtime` yourself to say so explicitly; the note is then not printed.

## The link prints "defines the allocator"

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated (RFC 0032)
```

A link input defines `malloc`, `calloc`, `realloc` or `free`, so the program keeps its own allocator. Guards on stack and global objects still work; guards on heap pointers pass, and no use-after-free or invalid free is caught at run time. To have the heap tracked, build without the program's allocator if it has a switch for that.

## The link prints "linking without the WeaveC runtime"

```text
weavec-cc: note: linking without the WeaveC runtime, but 'buffer.o' was compiled with it: the heap is untracked, its guards pass on it and releases are not validated (RFC 0032)
```

The link was given `-fno-weavec-runtime` (or something that implies it, such as a sanitizer) and at least one object was compiled without it. That object's guards are still in the program, but the image has no WeaveC allocator, so they find no heap object and pass. The program ledger says `"runtime": false` and its summary line counts those facets as "guardable (not enforced)". Use the same runtime flags for every compile and for the link.

## The program is much slower or uses more memory

The default mode links a runtime: every guarded operation looks its pointer up, and freed blocks are held in a 64 MiB quarantine before reuse. The measured cost on the project's benchmarks is 1.66 times the CPU time of a plain Clang build for cJSON, 1.85 for zlib and 5.94 for the Lua interpreter; code that spends its time in tight loops over pointers, as an interpreter does, is at the high end.

- Run with `WEAVEC_RT_STATS=1` to see how many lookups the program makes.
- Read `summary.guardedReasons` in the ledger. Each guarded facet that becomes proven or checked loses its guard: declare extents (`WEAVEC_COUNTED_BY`, `WEAVEC_ENDED_BY`, `WEAVEC_STRING`) for `unknown-extent`, and declare ownership for `unknown-callee`.
- Set `WEAVEC_RT_QUARANTINE=<bytes>` to shrink the quarantine if memory is the problem. A smaller quarantine catches fewer uses of freed blocks; `0` reuses blocks at once.
- Build the units that cannot pay with `-fno-weavec-runtime`. Their guardable facets become `unresolved` and are not enforced, and their cost returns to that of the checks alone (1.15, 1.00 and 1.09 times on the same benchmarks).

## A warning that "may" wording used to print is gone

In a `weavec-cc` build with the runtime, a possible temporal finding (`use of 'p' after it may have been freed`) is not printed when its facet is guarded: the guard traps if it happens. `-Wweavec-possible` prints these warnings again, and `weavec`, which enforces nothing, always prints them.

## The ledger has many unresolved rows

Read `summary.unresolvedReasons` in the ledger. In a default build a facet the analysis could not decide is `guarded` where a guard exists for it; what remains unresolved has no pointer for a guard to look up (the temporal facet of a call boundary, pointer arithmetic, casts) or a reason a guard does not address. With `-fno-weavec-runtime`, or after one of the fallback notes above, nothing is guarded and the count is higher. `unknown-callee` rows go away when the callee's definition is analysed with the caller (`weavec --whole-program`) or its declaration states its ownership; `unknown-extent` rows need a declared extent (`WEAVEC_COUNTED_BY`, `WEAVEC_ENDED_BY`, `WEAVEC_STRING`); `budget` rows name a function that exceeded the analysis budget (`-fweavec-budget`). See [adopt WeaveC incrementally](/guides/adoption/).

## A compile or link is slow

Each unit's analysis has a budget, `-fweavec-unit-budget=<n>` block transfers over all of its functions (by default 6 per site, at least 200,000); the functions analysed after it is spent get the over-budget defaults (`unresolved(budget)`, guarded where a guard applies). Lower it for faster builds of very large units, or set `0` for no limit. A default link analyses nothing; `-fweavec-link=analyze` does, within `-fweavec-link-budget=<seconds>` (default 120). When it runs out, or a group of units does not converge, the link goes on with a note and those units keep their compile-time results:

```text
weavec-cc: note: the whole-program analysis of 'parser.c' stopped at its budget; their compile-time results stand
```

## The link warns about an unanalyzed input

`unanalyzed-input` names the link inputs that have no valid WeaveC record: objects from another compiler, static archives, shared libraries, and objects whose record is stale. Calls into them are trusted. Rebuild the objects with `weavec-cc`; keep each `.o.weavec` record beside its object. Archives and shared libraries do not carry records yet.

## A require level rejects the build

`-fweavec-require=guarded` makes every unresolved operation an `unresolved-operation` error, `-fweavec-require=checked` makes every guarded operation one too (`… is guarded at run time only: …`), and `-fweavec-require=proven` also makes every runtime-checked operation an `unchecked-operation` error. The message names the reason. Resolve it as above or lower the level for that component. A spatial or null operation that is correct for reasons WeaveC cannot see can go in a reviewed `WEAVEC_UNSAFE` region: its facets become trusted, which every level allows.

## Report an issue

Include the WeaveC and LLVM versions, platform, exact command, a minimal source example, and the complete diagnostic or relevant report excerpt. Remove private source and environment details you do not intend to publish. Open a [bug report](https://github.com/weavefoundry/weavec/issues/new?template=bug_report.yml).
