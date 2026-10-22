---
title: Safety guarantees and scope
description: What a program compiled by weavec-cc is guaranteed at run time (accesses, bounds, releases, reuse only after a sweep, fortified sizes, poisoning), which memory the runtime tracks, the assumptions behind it, and what is and is not caught.
---

`weavec-cc` guards every memory access of the C code it compiles and removes a guard only where a simple local rule proves it redundant. The guarantee below is a property of the compiled program and its runtime. It is specified by [RFC 0036, _Soundness_](/rfcs/0036-deterministic-heap-reuse/#soundness), which keeps (A), (B) and (R) of [RFC 0035](/rfcs/0035-guard-by-default/#soundness) and adds (T), (F) and (P); the RFCs' _Implementation amendments_ are authoritative, and this page restates them.

## The guarantee

For a program whose every unit is compiled by `weavec-cc` in an enforcing mode (the default `-fweavec-checks=trap`, or `verify`; `report` only with `WEAVEC_RT_ABORT=1`), with the runtime linked, every memory access outside a `WEAVEC_UNSAFE` region satisfies:

- **(A) Accesses.** A load, store, atomic operation or memory intrinsic (`memcpy`, `memmove`, `memset`) of _w_ bytes at address _a_ executes only if every byte of [_a_, _a_ + _w_) is _addressable_ when it runs; otherwise the program traps before the access. The same holds for each memory argument of a call to a function in the [library table](https://github.com/weavefoundry/weavec/blob/main/lib/Core/LibrarySpec.txt), over the bytes the table says the call accesses, computed from the arguments, or, for a string, up to its terminator.
- **(B) Bounds.** An index `a[i]` into an array whose bound its type declares traps unless 0 ≤ `i` < _n_, or `i` ≤ _n_ where only the address is taken. A trailing array of a struct, which may be flexible, is excluded.
- **(R) Releases.** A release through the runtime's allocator (`free`, `realloc` and the rest of the family) releases null or the start of a live heap object; anything else stops the program.
- **(T) Temporal.** A byte of a released heap block is not addressable until a _sweep_ has found no pointer to the block in the program's stacks, registers, writable data and live heap. An access through a pointer the program held when the block was released therefore traps, however much it allocated since, and so does a second release of the block.
- **(F) Fortified sizes.** A fortified library call (`__strcpy_chk(d, s, 8)`, and the other `_chk` forms) writes no more than the size its `__builtin_object_size` argument states, or the program traps (`intra-object-overflow`).
- **(P) Poisoning.** A byte the program poisoned through AddressSanitizer's interface is not addressable until the program unpoisons it.

A function excluded from AddressSanitizer (`__attribute__((no_sanitize("address")))`, `no_sanitize_address`, `disable_sanitizer_instrumentation`) is treated as an unsafe region. Code inlined from an unsafe region stays unguarded wherever it lands.

## What is addressable

A byte is addressable unless the runtime knows it is not:

| Bytes                                               | Not addressable                                                                                                                |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| A heap object's slot in the runtime's allocator     | the bytes after the object's requested size; the whole slot once the object is released, until a sweep finds no pointer to it |
| A tracked local (in its function's frame)           | its redzones, always; the local before its scope begins and after it ends                                                      |
| An `alloca` block or a variable-length array        | its redzone                                                                                                                    |
| A global the unit defines (a _tracked_ global)      | its redzone                                                                                                                    |
| Bytes the program poisoned through ASan's interface | until the program unpoisons them                                                                                               |
| Low memory                                          | the lowest 64 KiB                                                                                                              |

Every other byte is addressable: memory the runtime does not track (other allocators, `mmap`, string literals, locals no guard can reach, frames and globals of code built without WeaveC), the bytes of live tracked objects, and a local's bytes after its function returned.

The runtime keeps one _shadow_ byte for every 16 bytes, saying how many of them are addressable or why none are. Its allocator puts every object in a slot of the smallest size class strictly larger than the object, so at least one byte after every heap object belongs to no object. A released block joins a _quarantine_ and is never reused from there: only a sweep, which stops the program's threads and scans its memory conservatively for pointers, moves the blocks it found no pointer to back to the free lists.

The locals a guard can reach (the _tracked_ locals: an access to them is guarded, or their address escapes) are laid out in one frame with a 64-byte redzone before the first and at least 32 bytes after each; in a frame that would be larger than 512 bytes, locals whose lifetimes never overlap (the arms of a `switch`) may share bytes, as Clang's own frames share them. Each `alloca` block and variable-length array gets a redzone after it. Each tracked global gets a redzone after it and a 32-byte alignment.

## What is caught

- **Heap overflow** that touches a byte past the block (`heap-buffer-overflow`), with a second line that places the address relative to the block. An access that jumps over the slot's tail into another live block lands on that block's bytes and passes.
- **Heap use after free and double free.** An access to a released block through any pointer the program still holds traps (`heap-use-after-free`), however much it allocated since, because the block is not reused while a pointer to it exists. A second `free`, a `free` of an interior, stack or global pointer, and a `realloc` of a released block stop the program (`weavec: invalid release of …`). The allocator validates every release in the image, from WeaveC's code or any other.
- **Stack and global overflow** into a redzone: an access that leaves a tracked local (`stack-buffer-overflow`), an `alloca` block or a variable-length array (`dynamic-stack-buffer-overflow`), or a tracked global (`global-buffer-overflow`) and touches its redzone.
- **Use after scope.** An access to a local after its block has ended traps (`stack-use-after-scope`), at every optimisation level, unless it lands in bytes that a local whose scope began later now shares (only in a frame larger than 512 bytes).
- **Overflow inside an object**, by an index into a declared array (B): `s.buf[i]` with `i` past `buf`'s declared bound traps (`index-out-of-bounds`) even when the bytes it would touch belong to the next field; and by a fortified string or `printf`-family call into a field (F): `strcpy(u->name, s)` into an 8-byte field that the C library's headers fortify traps (`intra-object-overflow`).
- **Null dereference.** An access at a small offset from null faults on the null page, or traps on the shadow of the lowest 64 KiB; one at a variable or large offset from a pointer not known to be non-null tests the pointer first (`null-dereference`).
- **Library calls.** A call in the library table whose memory arguments would be read or written outside their objects traps before the call: `memcpy(dst, src, n)` with `n` past either object, a `%s` argument of a `printf`-family call with a literal format that has no terminator in its object (`unterminated-string`), fortified forms (`__builtin___memcpy_chk`) as their plain forms. `strcpy`, `stpcpy`, `strcat`, `sprintf`, `vsprintf`, `snprintf` and `vsnprintf` go through the runtime's checked versions, which compute what the call writes before it writes and check the `printf`-family `%s` and `%n` arguments, through a `va_list` too; `gets` through one that checks each byte before it writes it; `memchr`, `strchr`, `strcmp`, `strncmp`, `strcasecmp` and `strncasecmp` through versions that check the bytes the call read up to where it stopped; the `scanf` family through versions that check, after the call, what each conversion that ran wrote; the `strto*` and `ato*` number parsers through versions that check the bytes the parse read. Overlapping operands of `memcpy` and of the other calls whose table row says they must not overlap trap as `overlapping-copy`.
- **Poisoned memory.** Bytes a program poisons through AddressSanitizer's interface (`ASAN_POISON_MEMORY_REGION`, as a pool allocator does for its free space) trap when a guarded access reaches them (`use-after-poison`). A program that sends its objects to `malloc` when it sees `__SANITIZE_ADDRESS__` (QuickJS does) gets its objects tracked one by one.

On Darwin the runtime's allocator also serves the C library's own allocations (`strdup`, `getline`, `asprintf`), so their blocks are tracked too.

## What is not caught

- **A jump over a redzone.** An index or pointer arithmetic that skips past a redzone or a slot's tail into another live object touches only addressable bytes and passes, as under AddressSanitizer. Bounds from each pointer's provenance are left to a later RFC (RFC 0036 measured a first design at twice the run time).
- **Use after return.** A local's bytes are addressable again once its function returns; an access through a pointer to it, or to a local of a frame `longjmp` abandoned, passes. Left to a later RFC.
- **A pointer jump inside one object by arithmetic.** A cursor walked from one field of a struct into the next stays inside its object; only indexes into declared arrays (B) and fortified calls (F) are checked against a field.
- **Hidden pointers.** A pointer the program stores only in a form the sweep does not recognise (XOR-linked, transformed, written to a file, held only by the kernel) does not keep its block from reuse (A6); a use of the block through it after a sweep reuses the block is not caught.
- **The program's own poisoning of a local or a global at a constant offset.** An access to a local or a global at a constant offset inside it needs no guard, so it is not checked against bytes the program poisoned there; accesses at a computed offset are.
- **Untracked memory.** Memory from another allocator (unless the program poisons it through ASan's interface), `mmap`, string literals, and objects of code built without WeaveC have no redzones; an access into them passes (A1). A write into a string literal passes its guard (the hardware usually faults on it).
- **Wide-character strings** passed to library functions (`wcslen`, `wcscpy`) are not scanned for their terminator; their counted arguments are guarded.
- **Functions outside the library table** are not checked at the call; their own accesses are guarded only if `weavec-cc` built them (A2).
- **Uninitialised pointers** in memory other than locals and the runtime's heap (which are zero) point anywhere; accesses through them are guarded, and one into untracked memory passes.
- **Data races.** A release by another thread between a guard and its access is not caught (A3).
- **Anything in a `WEAVEC_UNSAFE` region** or an AddressSanitizer-excluded function, and **any code not compiled by `weavec-cc`**: other compilers, precompiled libraries, C++ and Objective-C sources.
- **Leaks**, integer overflow as such, type confusion within an object, and inline assembly. WeaveC reports nothing before the program runs; a static analyser (such as the Clang static analyzer) finds leaks.

## Assumptions

|        | Assumption                                                                                                                                                                                                                                                                       |
| ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **A1** | An access to bytes the runtime does not track (memory from another allocator that the program does not poison through ASan's interface, `mmap`, string literals, code built without WeaveC) is to a live object it stays inside.                                                 |
| **A2** | A function in the library table accesses only what its row says. A function outside it is not checked at the call.                                                                                                                                                               |
| **A3** | No other thread releases memory between a guard and its access: the guard and the access are not atomic together.                                                                                                                                                                |
| **A4** | The program does not write the runtime's shadow or metadata except through guarded accesses.                                                                                                                                                                                     |
| **A5** | A frame left by `longjmp`, `pthread_exit` or another call that does not return leaves no poison behind: the stack's shadow below the caller is cleared before the call. A frame left by an exception unwinding through C frames keeps its poison until the stack is reused.      |
| **A6** | A pointer to a heap block is kept, while the program may still use it, as a whole aligned pointer in memory the sweep scans (any readable and writable mapping) or in a register.                                                                                                |
| **A7** | On Linux every thread accepts the sweep's signal within a second. A thread that blocks it holds up the reuse of released memory (nothing is reused until it accepts), never safety.                                                                                              |

No guard is removed because of what another function does, so nothing is assumed about callers, callees or the rest of the heap. The rules that remove guards are local to a function (an access inside a local or global at a known offset, an access a dominating guard on the same pointer covered with nothing between that may free, guards merged in a block, a loop whose whole range was found addressable before it ran), and `-fweavec-checks=verify` monitors them: it keeps every removed guard as a monitor that reports `weavec.proven` if it would have failed.

## False traps

A correct program does not trap. A program that breaks C's rules in a way that usually works does, as it does under AddressSanitizer:

- reading a word at a time past the end of a string or a buffer (a fast `strlen`, a hash over a key's last word);
- using a block's tail beyond its requested size (`malloc_usable_size` returns the requested size);
- indexing a multidimensional array past an inner bound (`int m[4][4]; m[0][k]` with `k` ≥ 4), which (B) checks.

`weavec-cc` compiles C as AddressSanitizer would (`__SANITIZE_ADDRESS__` is defined and `__has_feature(address_sanitizer)` is true), so code that keeps such reads from ASan behind those tests, or in a function marked `no_sanitize("address")`, needs no change. Other code needs a fix or a `WEAVEC_UNSAFE` region. A program that depends on the system allocator's identity (a `malloc_zone_t` of its own, allocator introspection, `mallopt`) sees WeaveC's allocator instead.

## Without the runtime

Nothing is enforced without the runtime. `weavec-cc` builds without it, and compiles without guards and without array-index checks, under `-fweavec-checks=none`, `-ffreestanding`, `-nostdlib` and its kin, a sanitizer that replaces the allocator (`-fsanitize=address` and the like), and on targets other than 64-bit Darwin and Linux; the link says so. A program that defines `malloc` itself keeps its allocator: its guards stay, but its heap is untracked, guards pass on it and releases are not validated. See [the runtime](/reference/cli/#the-runtime).
