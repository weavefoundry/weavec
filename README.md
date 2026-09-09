# WeaveC

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

A mostly source-compatible C compiler that brings Rust-style memory safety to existing C code through inferred ownership and borrowing. It automatically proves memory safety where possible, requires lightweight annotations only when necessary, and isolates truly unsafe operations behind explicit `unsafe` boundaries. The goal is to let teams incrementally make large C codebases memory-safe without rewriting them in Rust or abandoning the C ecosystem.

WeaveC is built on Clang/LLVM rather than implementing a compiler from scratch, using Clang for parsing, semantic analysis, diagnostics, optimization, code generation, and platform support. WeaveC adds its own ownership inference, borrow checking, lifetime analysis, and memory-safety rules on top, allowing the project to focus on its core innovation while remaining compatible with the existing C toolchain and ecosystem.

WeaveC itself is written in modern C++, which provides the most direct and complete access to Clang/LLVM's APIs and infrastructure. The core ownership, borrowing, lifetime, and inference logic should be kept as modular as possible so it remains cleanly separated from the Clang integration layer and can potentially be reused or extended in the future.

> **Checked code:** RFCs 0018–0019 add opt-in, compositional safety checking with
> `--checked`, `--checked-function=name`, and `WEAVEC_CHECKED`. Selected
> functions must establish bounds, validity and initialization, or state their
> entry requirements and trusted boundaries. Unresolved operations fail the
> check even when warnings are disabled. This is a conditional guarantee for
> selected source functions within the supported model. Checked memory facts
> compose through nested buffers, constructors, output parameters, complete
> fill/copy loops, and modeled string and allocation operations. See
> [checked code](docs/checked-code.md) for scope, reports and compiler use.

> **Status:** early. Function bodies are analyzed by a bounded dataflow ([RFC 0002](docs/rfcs/0002-intraprocedural-checking.md)): use-after-free and double-free through any alias and across loops, use-after-move, conflicting borrows, and pointers that outlive what they point to. Calls are modelled by inferred signatures ([RFC 0003](docs/rfcs/0003-signature-inference.md)): every function in the translation unit gets a summary of what it frees, writes, stores and returns, so `node_free(n); n->v` is caught without annotations; the C standard library and POSIX are covered by a shipped table, and annotations are checked against the bodies that carry them. Unsafe code has a boundary ([RFC 0004](docs/rfcs/0004-unsafe-boundaries.md)): pointers cast from integers or declared `WEAVEC_RAW` are *raw* and may only be dereferenced or released inside a `WEAVEC_UNSAFE` region, which is analysed rather than skipped; calls through function pointers are checked from the pointer type's annotations or from the functions assigned to it. Programs are analysed whole ([RFC 0005](docs/rfcs/0005-whole-program-analysis.md)): every translation unit exports the summaries of what it defines, so `other_free(o); o->v` is caught when `other_free` lives in another file, callbacks registered in one file are checked in the file that calls them, and unresolved direct or indirect calls are reported as boundaries. `weavec-cc` is a drop-in `cc` that does this as part of a normal build. The checker is precise where C idioms need it ([RFC 0006](docs/rfcs/0006-precision.md)): a borrow ends at the pointer's last use, not at the end of its scope; `if (p == sentinel) return; free(p);` knows the two are distinct; proven distinct array elements retain independent release history; and a function that frees its argument only when it returns `0` (or `NULL`) is summarised per outcome, so `if (rc != 0) free(p);` is clean while `if (rc == 0) use(p);` is reported. The other half of the ownership contract is checked too ([RFC 0007](docs/rfcs/0007-resource-lifecycle.md)): a resource that is never released is a `leak`, reported where it is lost (`if (c) return -1;` after a `malloc`, an overwrite, a discarded `strdup`, a `free(b)` that drops an owned `b->data`), and releasing it with the wrong function (`free` on a `FILE *`, through any wrapper, across files) is a `mismatched-release`. Pointers are checked for validity, not only ownership ([RFC 0008](docs/rfcs/0008-pointer-validity.md)): dereferencing a `malloc` result, a `strchr` result or any other pointer that may be null without testing it is a `null-dereference` (through calls too: a function that dereferences its parameter requires callers to prove it non-null), a pointer used before it is assigned is a `use-of-uninitialized`, `free` of a stack object, a string literal or the middle of an allocation is an `invalid-release`, and a callee that frees a value and then reinitialises the place (`realloc` in place, `free` then `= NULL`) still kills every copy of the old value the caller kept. The checker also knows *why* ([RFC 0009](docs/rfcs/0009-value-conditional-behaviour.md)): it tracks what is known about integers, so `if (c) free(p); ... if (!c) use(p);` and `switch (op) { case FREE: free(p); }` followed by another `switch` on `op` are clean; a callee's behaviour is summarised *per argument* (`l_alloc(ud, p, n, 0)` frees `p` and returns null, `l_alloc(ud, p, n, 64)` does not; `if (!b->noalloc) free(b->data)` frees only for callers that did not set the flag); and a function whose every path ends in `abort`, `exit`, `longjmp` or another such function is inferred `noreturn`, so `if (bad) die(); use(p);` is checked on the good path only. Objects with more than one owner are understood ([RFC 0010](docs/rfcs/0010-shared-ownership.md)): a reference count is inferred from the `obj_ref`/`obj_unref` pair that keeps it (`o->rc++`; `if (--o->rc == 0) free(o)`, in any spelling from `o->rc--` to `__atomic_fetch_sub`), so `b = obj_ref(a); obj_unref(b); use(a)` is clean while one `obj_unref` too many is a `double-free`, a use after the last one is a `use-after-free`, and a reference taken and dropped is a `leak`; a callee that stores its argument only on success (`if (bag_put(b, s) < 0) free(s);`) is summarised per outcome, and one that keeps its argument in a node of its own (`table_set(t, o)`) is known to have kept it. Memory is checked spatially as well as temporally ([RFC 0011](docs/rfcs/0011-spatial-safety.md)): a pointer is an object and an offset into it, so `free(container_of(i, struct outer, in))` frees what `i` belongs to and a field pointer kept across `free(p)` is a `use-after-free`; objects have a size (`malloc(n)`, `char buf[8]`, a wrapper's `xmalloc(n)`, a `WEAVEC_SIZED_BY(len)` parameter) and every subscript, dereference and `memcpy`/`memset`/`fgets`/`read` length is checked against it, through what the path knows of the index (`i <= n` on `malloc(n)` may reach one past the end; `i < 8` on four bytes may reach `7`), so `buf[8]`, `for (i = 0; i <= n; i++) p[i]` and `memcpy(small, src, 16)` are `out-of-bounds`, and a callee that writes `b[7]` requires eight bytes of every caller. Strings and counted fields are sizes too ([RFC 0012](docs/rfcs/0012-spatial-safety-strings-and-fields.md)): the checker knows the length of what a buffer holds (`strlen(s)`, a literal, what `strcpy`/`strcat`/`sprintf` left) and whether it is NUL-terminated at all, so `strcpy(malloc(strlen(s)), s)`, `strcat(buf, "d")` on a full `buf` and `strlen(name)` after a `strncpy` that filled `name` are `out-of-bounds`; a pointer field is sized by a sibling count, declared (`char *WEAVEC_SIZED_BY(cap) data; size_t cap;`) or inferred from every store the program makes into it, so `b->data[b->cap]` is `out-of-bounds` and `b->data = malloc(4); b->cap = 8;` is an `annotation-mismatch`; `if (i <= n - 1) a[i + 1]` and `if (i >= 8) buf[i]` are decided; and `WEAVEC_ASSUME(len < cap)` states an invariant the function cannot see. A Juliet-style recall set (`test/recall`) tracks what fraction of each CWE the checker catches. Shipped summaries for libraries beyond libc and a Clang plugin packaging are next. See [docs/roadmap.md](docs/roadmap.md).

Constructor inference now carries the initialized heap back to callers
([RFC 0013](docs/rfcs/0013-interprocedural-heap-state.md)). If `box_new()`
allocates four bytes for `b->data`, `b->data[4]` is checked in its caller,
and `free(b)` without releasing that child reports a leak. Shared children,
argument aliases, record results and out-parameters retain their identities
across calls and files. Changing the variable used as an allocation size
does not change the allocation's bounds. The
[fixed evaluation set](test/evaluation/README.md) includes known misses;
unknown bounds and incomplete heap descriptions remain gaps in coverage.
The [corpus notes](scripts/corpus/README.md) record Lua’s remaining GC and
callback limitations, including the precision and performance tradeoffs.

Pointer and call identity is preserved by [RFC 0014](docs/rfcs/0014-pointer-identity-and-call-effects.md).
Indirect calls use the function values that reach the call. Callback helpers
are checked under bounded bindings of those values, including across compiler
sidecars. Pointer equality and inequality guard callee effects, and complete
`memcpy`/`memmove` copies retain pointer ownership and aliases. Record paths
carry layout information so unrelated views do not acquire fabricated fields.
Unsupported copies, incompatible views and exhausted analysis limits are
visible through `analysis-incomplete`; unknown callbacks retain the existing
annotation or strict-mode boundary. The absence of diagnostics is not a
verification certificate for code outside these supported models. The
[validation report](docs/validation-rfc0014.md) records the fixed evaluation,
corpus coverage changes and increased whole-program analysis cost.

Arrays and containers now use selected element identities ([RFC 0015](docs/rfcs/0015-array-and-container-ownership.md)). Releasing `a[1]` preserves an earlier release of `a[0]`; initialization, nullness, callback targets and ownership belong to individual pointer or record cells. Complete array `memcpy`/`memmove` operations preserve pointee identity, including overlapping moves and bounded symbolic ranges. Selected effects, range copies, returned containers and proved fill/cleanup loops compose through summaries and compiler sidecars. The representation tracks up to 32 cells and 32 range facts per storage object; unresolved selections, unsupported compositions and exhausted limits report incomplete coverage. The [validation report](docs/validation-rfc0015.md) records the improved fixed evaluation alongside corpus false positives and increased analysis cost. This remains an early static checker with explicit coverage limits.

Helpers now preserve the safety meaning of related pointer arguments
([RFC 0016](docs/rfcs/0016-compositional-call-checking.md)). Given
`release_then_write(p, p)`, the checker follows the callee's statement order
and reports its use-after-free. Reversing the operations remains clean.
The same checking covers aliased output storage, shared record children,
selected elements, callbacks and separate compiler objects. Contexts retain
bounded caller facts; unavailable projections report incomplete coverage.
The [validation report](docs/validation-rfc0016.md) records both the added
detections and the cost and coverage warnings on real code. Calls without
established interacting identities still use generic summaries; a quiet run
does not establish that arbitrary inputs are disjoint.

The current integer and spatial milestone
([RFC 0017](docs/rfcs/0017-c-integer-semantics-and-spatial-safety.md)) uses
the target's integer widths, promotions and conversions when checking paths,
ownership effects and buffer sizes. Narrowing, `_Bool`, mixed signedness and
unsigned wrap retain their C meaning; a definitely invalid supported operation
reports `invalid-integer-operation`. Bounded symbolic products, minimum bounds,
numeric returns and out-parameters carry sizes and conditional access
requirements through helpers and separate compiler objects. Supported
variable-length arrays retain their declaration-time bounds. Side-effecting
dimensions such as `char a[n++]` conservatively lose their captured bounds
and warn with `analysis-incomplete`; their extent and `sizeof` are not treated
as proved. Flexible-array tails use the backing allocation and target field
layout.

`--dump-analysis` distinguishes spatial checks that are `proven`, a `violation`
or `unresolved`. Unknown bounds, unsupported expressions and exhausted limits
remain coverage gaps; general nonlinear and loop reasoning are outside the
model. Early-exit and other unsupported loops do not produce inferred
must-requirements on callers. Existing annotations remain trusted contracts.
There is no runtime instrumentation or whole-program verification certificate.
Core summary format is **15**; sidecar format **16** requires rebuilding objects carrying older
sidecars. The [validation report](docs/validation-rfc0017.md) records
**900/900 tests passing**, including under ASan/UBSan, **44/44 original bugs
detected and 32/32 clean cases**, plus twelve separate bug/clean regression
pairs. Three repeated corpus runs show the cost: median analysis time grew
from 141 to 336 seconds and peak memory grew 19%, exceeding the RFC targets.
Three new Jansson false positives and remaining coverage gaps are documented.

Analysis can retain parsed units and immutable function preparation, reuse
specializations according to their dependencies, and share checked explanations
([RFC 0020](docs/rfcs/0020-scalable-modular-checked-analysis.md)). Optional
`--analysis-cache` checkpoints avoid function dataflow for unchanged reusable
units; `--analysis-stats` makes that work visible. Compact checked reports keep
source obligations and call routes in shared tables. See the
[incremental analysis guide](docs/incremental-analysis.md) for both command-line
spellings, input validation, report decoding and conservative cache misses.
The [RFC 0020 validation](docs/validation-rfc0020.md) records 1,029/1,029 tests
passing in Debug and ASan/UBSan, 43% less ordinary analysis time and 5% less
peak memory. All five checked corpus reports finish within 600 seconds; warm
runs reuse every unit with zero function analyses and equivalent reports.
Incomplete functions and existing checking limits remain visible.

## Quick look

```c
#include <stdint.h>
#include <stdlib.h>
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(size_t n);
size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);

struct node { int v; struct node *next; };
static void node_free(struct node *n) { free(n); }   // inferred: consumes n

int example(struct node *n) {
  struct node *m = n;
  node_free(m);
  return n->v;       // error: use of 'n' after it was freed [weavec::use-after-free]
}

int *escape(void) {
  int x = 0;
  return &x;         // error: returned pointer may outlive 'x' [weavec::lifetime-too-short]
}

struct node *WEAVEC_OWNED from_handle(uintptr_t h) {
  WEAVEC_UNSAFE { return (struct node *)h; }   // asserts ownership, at one greppable point
}

int handle(uintptr_t h) {
  struct node *r = (struct node *)h;             // r is raw: no one knows who owns it
  return r->v;       // error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
}
```

```
$ weavec example.c --
example.c:13:10: error: use of 'n' after it was freed [weavec::use-after-free]
   13 |   return n->v;
      |          ^
example.c:12:3: note: freed here (through 'm')
   12 |   node_free(m);
      |   ^
example.c:18:10: error: returned pointer may outlive 'x', which it points to [weavec::lifetime-too-short]
   18 |   return &x;
      |          ^
example.c:17:7: note: 'x' is declared here
   17 |   int x = 0;
      |       ^
example.c:27:10: error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
   27 |   return r->v;
      |          ^
example.c:26:20: note: 'r' is raw: cast from an integer here
   26 |   struct node *r = (struct node *)h;
      |                    ^
example.c:27:10: note: move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first
```

Annotations (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_UNSAFE`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_RETAINS`, `WEAVEC_RELEASES`, `WEAVEC_REFCOUNT`, `WEAVEC_OWNED_BY(f)`) expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## Using it as the compiler

`weavec-cc` is Clang's driver with WeaveC inside. Point a build at it and it compiles as `clang` would, analyses each file as it compiles it, and checks the whole program when it links:

```sh
$ CC=weavec-cc make
weavec-cc -c node.c -o node.o          # node.o and node.o.weavec (its summaries)
weavec-cc -c main.c -o main.o
weavec-cc node.o main.o -o prog        # reads the sidecars, analyses the program, then links
main.c:8:3: error: 'n' is freed twice [weavec::double-free]
    8 |   node_free(n);
      |   ^
main.c:7:3: note: previously freed here
    7 |   node_free(n);
      |   ^
1 error generated.
```

A bug inside one file is reported when that file is compiled; a bug that needs two files (`node_free` is defined in `node.c`) is reported when they are linked, and an error stops the link. Flags: `-fno-weavec` (compile only), `-fweavec-strict` (every call into unknown code is a raw operation), `-fno-weavec-link` (skip the link-time step), `-Wno-weavec-annotation-required`, `-Wno-error=weavec-use-after-free` (lower an error to a warning while migrating), `-Werror=weavec`.

The tooling form analyses a compilation database without building: `weavec --whole-program -p build/` (all sources) or `weavec --whole-program a.c b.c -- -Iinclude`. Without `--whole-program`, `weavec file.c --` checks one file as before.

## Building

Requirements: CMake ≥ 3.24, Ninja, a C++20 compiler, and an LLVM/Clang development install (23.x recommended; ≥ 20 supported). Tests additionally need `lit` and LLVM's `FileCheck`.

```sh
# macOS
brew install llvm ninja lit
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"

# Ubuntu / Debian
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 23 all
sudo apt-get install -y ninja-build && pip install lit
export WEAVEC_LLVM_PREFIX=/usr/lib/llvm-23

# Everyone
cmake --preset dev          # configure into build/dev
cmake --build --preset dev  # build
ctest --preset dev          # run unit + integration tests
```

Other presets: `dev-asan`, `dev-tidy`, `release`, `relwithdebinfo`. The full list is in [`CMakePresets.json`](CMakePresets.json); the developer guide is [docs/development.md](docs/development.md).

## Releases

[GitHub Releases](https://github.com/weavefoundry/weavec/releases) is the
distribution channel for WeaveC. Initial 0.x releases provide
`weavec-X.Y.Z-source.tar.gz` and `SHA256SUMS`. They are early checker releases
with the coverage limits described above; APIs and on-disk formats can change
between minor versions.

The first release creates `CHANGELOG.md` from Conventional Commits; later
releases regenerate it from the commit history.
The earlier hand-written implementation and migration notes are preserved in
[the development history](docs/development-history.md).

Download both files into the same directory and verify the archive with
`shasum -a 256 -c SHA256SUMS` (or `sha256sum -c SHA256SUMS` on Linux).
Install the build requirements above, extract the archive, and run from its
`weavec-X.Y.Z` directory:

```sh
cmake --preset release -DWEAVEC_VERSION_SUFFIX=""
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
weavec --version
weavec-cc --version
```

Keep the LLVM/Clang installation used for the build available at runtime.
Prebuilt portable binaries and package-manager distribution are future work;
the current compiler records LLVM resource and executable paths at configure
time. The [release workflow](docs/development.md#releasing) describes version
selection, publication and retries.

## Repository layout

```
include/weavec/   Public C++ headers
  Core/           Ownership lattice, lifetimes, borrows, moves, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts; the checkers
  Frontend/       Clang FrontendAction / libTooling integration
lib/              Implementations, mirroring include/
tools/weavec/     The analysis tool (libTooling; --whole-program for a compilation database)
tools/weavec-cc/  The drop-in compiler driver (Clang's driver with WeaveC inside)
resources/        weavec.h, the C-facing annotation header (installed to lib/weavec/include)
unittests/        GoogleTest unit tests
test/             lit + FileCheck integration tests
docs/             Architecture, RFCs (docs/rfcs/), roadmap
cmake/            Build-system modules
```

The layering rule is strict: `Core` must not include anything from `clang/` or `llvm/`. `Analysis` is the only layer that knows about both worlds. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) first. Security issues should be reported privately as described in [SECURITY.md](SECURITY.md).

## License

Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
