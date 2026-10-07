---
title: Safety guarantees and scope
description: What a translation unit compiled by weavec-cc is guaranteed at run time, which bytes the runtime knows are not addressable, the assumptions behind it, what is and is not caught, and what the advisory analysis adds.
---

`weavec-cc` guards every memory access of the C code it compiles and removes a guard only where a simple local rule proves it redundant. The guarantee below is a property of the compiled program and its runtime; nothing the ownership analysis concludes is part of it. It is specified by [RFC 0035, _Soundness_](/rfcs/0035-guard-by-default/#soundness), with its _Implementation amendments_, which are authoritative; this page restates them.

## The guarantee

For a translation unit compiled by `weavec-cc` in an enforcing mode (the default `-fweavec-checks=trap`, or `verify`; `report` only with `WEAVEC_RT_ABORT=1`), with the runtime linked, every memory access the unit's code makes outside a `WEAVEC_UNSAFE` region satisfies:

- **(A) Accesses.** A load, store, atomic operation or memory intrinsic (`memcpy`, `memmove`, `memset`) of _w_ bytes at address _a_ executes only if every byte of [_a_, _a_ + _w_) is _addressable_ when it runs; otherwise the program traps before the access. The same holds for each memory argument of a call to a function in the [library table](https://github.com/weavefoundry/weavec/blob/main/lib/Core/LibrarySpec.txt), over the bytes the table says the call accesses, computed from the arguments, or, for a string, up to its terminator.
- **(B) Bounds.** An index `a[i]` into an array whose bound its type declares traps unless 0 ≤ `i` < _n_, or `i` ≤ _n_ where only the address is taken. A trailing array of a struct, which may be flexible, is excluded.
- **(R) Releases.** A release through the runtime's allocator (`free`, `realloc` and the rest of the family) releases null or the start of a live heap object; anything else stops the program.

A function excluded from AddressSanitizer (`__attribute__((no_sanitize("address")))`, `no_sanitize_address`, `disable_sanitizer_instrumentation`) is treated as an unsafe region. Code inlined from an unsafe region stays unguarded wherever it lands.

## What is addressable

A byte is addressable unless the runtime knows it is not:

| Bytes                                                       | Not addressable                                                                                          |
| ----------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| A heap object's slot in the runtime's allocator             | the bytes after the object's requested size; the whole slot while the object is released and quarantined |
| A local whose address a guard can reach (a _tracked_ local) | its redzones, always; the local itself before its scope begins and after it ends                         |
| A global the unit defines (a _tracked_ global)              | its redzone                                                                                              |
| Low memory                                                  | the lowest 64 KiB                                                                                        |

Every other byte is addressable: memory the runtime does not track (other allocators, `mmap`, string literals, locals no guard can reach, frames and globals of code built without WeaveC) and the bytes of live tracked objects.

The runtime keeps one _shadow_ byte for every 16 bytes, saying how many of them are addressable or why none are. Its allocator puts every object in a slot of the smallest size class strictly larger than the object, so at least one byte after every heap object belongs to no object, and holds released blocks in a quarantine (16 MiB by default, `WEAVEC_RT_QUARANTINE=<bytes>`) before it reuses them. `weavec-cc` lays out the tracked locals of a function in one frame, each at a 32-byte boundary followed by a redzone of at least 32 bytes (more for large objects: an eighth of the object, up to 4 KiB), after a 64-byte left zone, and gives each tracked global a redzone of the same size.

## What is caught

- **Heap overflow.** An access that leaves a heap block forwards traps at its first byte past the object: `heap-buffer-overflow`, with a second line that places the address relative to its block.
- **Heap use after free and double free.** An access to a released block traps while the block is in the quarantine (`heap-use-after-free`); a second `free`, a `free` of an interior, stack or global pointer, and a `realloc` of a released block stop the program (`weavec: invalid release of …`). The allocator validates every release in the image, from WeaveC's code or any other.
- **Stack and global overflow.** An access that leaves a tracked local or global into its redzone traps (`stack-buffer-overflow`, `global-buffer-overflow`); so does one past an `alloca` block or a variable-length array (`dynamic-stack-buffer-overflow`).
- **Use after scope.** An access to a local after its block has ended traps (`stack-use-after-scope`), at every optimisation level.
- **Index overflow inside an object.** By (B), `s.buf[i]` with `i` past `buf`'s declared bound traps (`index-out-of-bounds`) even when the bytes it would touch belong to the next field.
- **Null dereference.** An access at a small offset from null faults on the null page, or traps on the shadow of the lowest 64 KiB; one at a variable or large offset from a pointer not known to be non-null tests the pointer first (`null-dereference`).
- **Library calls.** A call in the library table whose memory arguments would be read or written outside their objects traps before the call: `memcpy(dst, src, n)` with `n` past either object, a `%s` argument of a `printf`-family call with a literal format that has no terminator in its object (`unterminated-string`), fortified forms (`__builtin___memcpy_chk`) as their plain forms. `strcpy`, `stpcpy`, `strcat`, `sprintf`, `vsprintf`, `snprintf` and `vsnprintf` go through the runtime's checked versions, which compute what the call writes before it writes and check the `printf`-family `%s` and `%n` arguments, through a `va_list` too; `gets` through one that checks each byte before it writes it; `memchr`, `strchr`, `strcmp`, `strncmp`, `strcasecmp` and `strncasecmp` through versions that check the bytes the call read up to where it stopped; the `scanf` family through versions that check, after the call, what each conversion that ran wrote. Overlapping operands of `memcpy` and of the other calls whose table row says they must not overlap trap as `overlapping-copy`.

On Darwin the runtime's allocator also serves the C library's own allocations (`strdup`, `getline`, `asprintf`), so their blocks are tracked too.

## What is not caught

- **An access that jumps over a redzone.** An access that skips past a heap slot's tail into another live object, reaches backwards into the previous slot's object, or jumps over a local's or a global's redzone into another object, passes: only the bytes accessed are looked at, not the object the pointer came from.
- **A use after free after the quarantine.** Once a released block's storage is reused, a stale pointer sees the new object.
- **Use after return.** A frame's shadow is cleared when its function returns, because frames of code built without WeaveC reuse the stack.
- **Overflow inside an object by pointer arithmetic or a library call.** A cursor walked from one field into the next, or a `strcpy` into a field bounded only by its type, stays inside the object; only indexes into declared arrays are checked against the type (B).
- **Untracked memory.** Memory from another allocator, `mmap`, string literals, and objects of code built without WeaveC have no redzones and no shadow; an access into them passes (A1). A write into a string literal passes its guard (the hardware usually faults on it).
- **Wide-character strings** passed to library functions (`wcslen`, `wcscpy`) are not scanned for their terminator; their counted arguments are guarded.
- **Functions outside the library table** are not checked at the call; their own accesses are guarded only if `weavec-cc` built them (A2).
- **Uninitialised pointers** in memory other than locals and the runtime's heap (which are zero) point anywhere; accesses through them are guarded, and one into untracked memory passes.
- **Data races.** A release by another thread between a guard and its access is not caught (A3).
- **Anything in a `WEAVEC_UNSAFE` region** or an AddressSanitizer-excluded function, and **any code not compiled by `weavec-cc`**: other compilers, precompiled libraries, C++ and Objective-C sources.
- **Leaks**, integer overflow as such, type confusion within an object, and inline assembly. Leaks are reported only by the [advisory analysis](#the-advisory-analysis).

## Assumptions

|        | Assumption                                                                                                                                                                                                                                                                                                                                          |
| ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **A1** | An access to bytes the runtime does not track (memory from another allocator, `mmap`, string literals, code built without WeaveC) is to a live object it stays inside.                                                                                                                                                                              |
| **A2** | A function in the library table accesses only what its row says. A function outside it is not checked at the call.                                                                                                                                                                                                                                  |
| **A3** | No other thread releases memory between a guard and its access: the guard and the access are not atomic together.                                                                                                                                                                                                                                   |
| **A4** | The program does not write the runtime's shadow or metadata except through guarded accesses.                                                                                                                                                                                                                                                        |
| **A5** | A frame left by a non-local jump from code without WeaveC (an exception unwinding through C frames) may keep its redzones until a WeaveC frame next uses that stack, and a guard that meets one can trap. A frame left by `longjmp`, `siglongjmp`, `pthread_exit` or another `noreturn` call from WeaveC-built code has its redzones cleared first. |

No guard is removed because of what another function does, so nothing is assumed about callers, callees or the rest of the heap. The rules that remove guards are local to a function (an access inside a local or global at a known offset, an access a dominating guard on the same pointer covered with nothing between that may free, guards merged in a block, a loop whose whole range was found addressable before it ran), and `-fweavec-checks=verify` monitors them: it keeps every removed guard as a monitor that reports `weavec.proven` if it would have failed.

## False traps

A correct program does not trap. A program that breaks C's rules in a way that usually works does, as it does under AddressSanitizer:

- reading a word at a time past the end of a string or a buffer (a fast `strlen`, a hash over a key's last word);
- using a block's tail beyond its requested size (`malloc_usable_size` returns the requested size);
- indexing a multidimensional array past an inner bound (`int m[4][4]; m[0][k]` with `k` ≥ 4), which (B) checks;
- scanning the stack conservatively, as a garbage collector does, which reads redzones.

Such code needs a fix or a `WEAVEC_UNSAFE` region; a function the project already excludes from AddressSanitizer needs nothing. A program that depends on the system allocator's identity (a `malloc_zone_t` of its own, allocator introspection, `mallopt`) sees WeaveC's allocator instead. Exceptions unwinding through C frames are covered by A5.

## Without the runtime

Nothing is enforced without the runtime except (B). `weavec-cc` builds without it, and its compiles without guards, under `-fweavec-checks=none`, `-ffreestanding`, `-nostdlib` and its kin, a sanitizer that replaces the allocator (`-fsanitize=address` and the like), and on targets other than 64-bit Darwin and Linux; the link says so. A program that defines `malloc` itself keeps its allocator: its guards stay, but its heap is untracked, guards pass on it and releases are not validated. See [the runtime](/reference/cli/#the-runtime).

## The advisory analysis

The ownership and lifetime analysis is not part of the guarantee: it removes no guard and adds none. It reports bugs before the program runs, in the `weavec` tool (errors for definite findings) and under `weavec-cc -fweavec-diagnose` (warnings):

- use of a released or moved object, a double release, releasing non-heap or interior storage, and releasing through the wrong allocator family;
- dereferencing a pointer that is null on every path;
- an access past an object of exact extent for every value the facts allow, including library calls with a known required length, overlapping copies and writes into string literals;
- a returned or stored pointer that outlives its object;
- leaks, a contradicted `WEAVEC_ASSUME`, and a declaration whose annotation contradicts its definition, across translation units under `weavec --whole-program`.

A definite finding is an error only once the analysis has found a feasible path that reaches it; a finding on some paths only is a warning with "may" wording. A build fails on them only if you ask: run `weavec` in CI, or build with `-fweavec-diagnose -Werror=weavec`. The analysis can be wrong in both directions; a guard is what stops a bug at run time.
